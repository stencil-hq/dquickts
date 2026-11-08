/*
 * dquickts Python C API (plan D5, unit B): type objects, slot bridges,
 * PyType_*, descriptors, method objects, functions, struct sequences.
 *
 * Contract: python/capi.h, local://capi-design.md section 2 ("Types").
 * Ported from CPython 3.15 Objects/typeobject.c, Objects/descrobject.c,
 * Objects/methodobject.c, Objects/classobject.c, Objects/funcobject.c,
 * Objects/structseq.c, Python/structmember.c, Python/slots.c and the
 * generated slot tables (PSF-2.0).
 *
 * Type kinds handled here (QPCapiTypeInfo.origin):
 *  - BUILTIN: static PyTypeObject bound to a QPType of the runtime
 *    (QP_CAPI_STATIC_TYPES, exceptions of E1, contextvars types of E2).
 *    Its slots are reverse bridges (C call -> QP slot); the ones C code
 *    calls on an explicit base are per-type ("dedicated") bridges.
 *  - CSTATIC / CHEAP: C-defined types (PyType_Ready of a static type,
 *    PyType_FromSpec* heap types).  Their QP slots are forward bridges
 *    (QP call -> C slot), the C slots are the extension's functions.
 *  - MIRROR: PyHeapTypeObject mirror (kind TYPE) of any other QPType
 *    (Python classes, runtime types without a static object).  Slots
 *    inherited from a C type keep the C function, the others are generic
 *    reverse bridges.
 * Forward bridges dispatch on the dynamic type: the nearest C-defined type
 * in the MRO of the object's type whose QP slot is that forward bridge.
 */

/* ========================================================================= */
/* Private state                                                             */
/* ========================================================================= */

enum {
    QP_CAPI_TO_BUILTIN = 1,     /* static object bound to a runtime type */
    QP_CAPI_TO_CSTATIC,         /* static C type readied by PyType_Ready */
    QP_CAPI_TO_CHEAP,           /* heap type from PyType_FromSpec & co */
    QP_CAPI_TO_MIRROR,          /* kind TYPE mirror of a runtime/Python type */
};

#define QP_CAPI_TI_MAGIC 0x51544942u    /* "BITQ" */
#define QP_CAPI_NDEDICATED 192          /* static types with dedicated bridges */

/* B's per-type record, kept in PyTypeObject.tp_subclasses (unused by the
   mirror model; CPython keeps the subclass dict there). */
typedef struct QPCapiTypeInfo {
    uint32_t magic;
    uint8_t origin;             /* QP_CAPI_TO_* */
    int16_t dedicated;          /* dedicated reverse bridge index, -1 none */
    PyTypeObject *tp;
    QPType *qt;                 /* bound runtime type (also tp_cache) */
    JSValue j;                  /* static types: owned reference to the type object */
    PyObject *holder;           /* pin holder for borrowed results (static types) */
    QPTypeSpec spec;            /* C-defined types: spec given to qp_type_new_from_spec_bases */
    QPMethodDef *methods;       /* spec arrays (owned) */
    QPGetSetDef *getsets;
    char *name_buf;             /* owned tp_name of mirrors */
    char *doc_buf;              /* owned tp_doc of heap types */
    uint64_t c_flags;           /* C-only tp_flags bits to keep on refresh */
    /* suites of static types that have none of their own */
    PyAsyncMethods as_async;
    PyNumberMethods as_number;
    PySequenceMethods as_sequence;
    PyMappingMethods as_mapping;
    PyBufferProcs as_buffer;
} QPCapiTypeInfo;

static QPCapiTypeInfo *qp_capi_tinfo(PyTypeObject *tp)
{
    QPCapiTypeInfo *ti = (QPCapiTypeInfo *)tp->tp_subclasses;
    return (ti && ti->magic == QP_CAPI_TI_MAGIC && ti->tp == tp) ? ti : NULL;
}

/* ---- pointer-pair hash map (process lifetime, context-free) ------------- */

typedef struct QPCapiPMapEntry {
    const void *a, *b;          /* key; a == NULL: empty, a == (void*)1: deleted */
    void *value;
} QPCapiPMapEntry;

typedef struct QPCapiPMap {
    QPCapiPMapEntry *tab;
    size_t size, count, used;   /* size: power of two; used counts tombstones */
} QPCapiPMap;

#define QP_CAPI_PMAP_DELETED ((const void *)1)

static size_t qp_capi_pmap_hash(const void *a, const void *b)
{
    uint64_t h = (uint64_t)(uintptr_t)a * 0x9E3779B97F4A7C15ull;
    h ^= (uint64_t)(uintptr_t)b * 0xC2B2AE3D27D4EB4Full;
    h ^= h >> 29;
    return (size_t)h;
}

static void *qp_capi_pmap_get(QPCapiPMap *m, const void *a, const void *b)
{
    size_t i, mask;
    if (!m || !m->tab)
        return NULL;
    mask = m->size - 1;
    for (i = qp_capi_pmap_hash(a, b) & mask;; i = (i + 1) & mask) {
        QPCapiPMapEntry *e = &m->tab[i];
        if (!e->a)
            return NULL;
        if (e->a == a && e->b == b)
            return e->value;
    }
}

static int qp_capi_pmap_resize(QPCapiPMap *m, size_t nsize)
{
    QPCapiPMapEntry *old = m->tab, *nt;
    size_t i, osize = m->size;
    nt = qp_sys_calloc(nsize, sizeof(*nt));
    if (!nt)
        return -1;
    m->tab = nt;
    m->size = nsize;
    m->count = m->used = 0;
    for (i = 0; i < osize; i++) {
        QPCapiPMapEntry *e = &old[i];
        if (e->a && e->a != QP_CAPI_PMAP_DELETED) {
            size_t j, mask = nsize - 1;
            for (j = qp_capi_pmap_hash(e->a, e->b) & mask; nt[j].a; j = (j + 1) & mask)
                ;
            nt[j] = *e;
            m->count++;
            m->used++;
        }
    }
    qp_sys_free(old);
    return 0;
}

/* 0, -1 (out of memory, no exception set) */
static int qp_capi_pmap_set(QPCapiPMap *m, const void *a, const void *b, void *value)
{
    size_t i, mask, tomb = (size_t)-1;
    if ((m->used + 1) * 2 > m->size &&
        qp_capi_pmap_resize(m, m->size ? (m->count * 4 > m->size ? m->size * 2 : m->size) : 64) < 0)
        return -1;
    mask = m->size - 1;
    for (i = qp_capi_pmap_hash(a, b) & mask;; i = (i + 1) & mask) {
        QPCapiPMapEntry *e = &m->tab[i];
        if (!e->a)
            break;
        if (e->a == QP_CAPI_PMAP_DELETED) {
            if (tomb == (size_t)-1)
                tomb = i;
            continue;
        }
        if (e->a == a && e->b == b) {
            e->value = value;
            return 0;
        }
    }
    if (tomb != (size_t)-1) {
        i = tomb;
    } else {
        m->used++;
    }
    m->tab[i].a = a;
    m->tab[i].b = b;
    m->tab[i].value = value;
    m->count++;
    return 0;
}

static void qp_capi_pmap_del(QPCapiPMap *m, const void *a, const void *b)
{
    size_t i, mask;
    if (!m || !m->tab)
        return;
    mask = m->size - 1;
    for (i = qp_capi_pmap_hash(a, b) & mask;; i = (i + 1) & mask) {
        QPCapiPMapEntry *e = &m->tab[i];
        if (!e->a)
            return;
        if (e->a == a && e->b == b) {
            e->a = QP_CAPI_PMAP_DELETED;
            e->b = NULL;
            e->value = NULL;
            m->count--;
            return;
        }
    }
}

/* qp_capi.type_map: QPType* -> PyTypeObject* of C-defined and static types */
struct QPCapiTypeMap {
    QPCapiPMap map;
};

static struct QPCapiTypeMap qp_capi_type_map_store;    /* qp_capi.type_map */
#define qp_capi_type_pmap (qp_capi_type_map_store.map)

static PyTypeObject *qp_capi_type_map_get(QPType *t)
{
    return (PyTypeObject *)qp_capi_pmap_get(&qp_capi_type_pmap, t, NULL);
}

/* ---- method / getset / member registry ---------------------------------- */

enum {
    QP_CAPI_DEF_METHOD,         /* PyMethodDef (cls = defining class or NULL) */
    QP_CAPI_DEF_GETSET,         /* PyGetSetDef */
    QP_CAPI_DEF_MEMBER,         /* PyMemberDef */
    QP_CAPI_DEF_WRAPPER,        /* struct wrapperbase + wrapped (PyDescr_NewWrapper) */
};

typedef struct QPCapiDef {
    uint8_t kind;               /* QP_CAPI_DEF_* */
    union {
        PyMethodDef *ml;
        PyGetSetDef *gs;
        PyMemberDef *m;
        struct wrapperbase *wb;
    } u;
    PyTypeObject *cls;          /* METH_METHOD defining class */
    void *wrapped;              /* QP_CAPI_DEF_WRAPPER */
    QPMethodDef qml;            /* QP wrappers (process lifetime) */
    QPGetSetDef qgs;
} QPCapiDef;

struct QPCapiRegistry {
    QPCapiDef **defs;           /* index = QP magic */
    int count, size;
    QPCapiPMap dedup;           /* (C def, cls/kind) -> index + 1 */
};

static struct QPCapiRegistry qp_capi_registry;

static QPCapiDef *qp_capi_def_at(int magic)
{
    if (magic < 0 || magic >= qp_capi_registry.count)
        return NULL;
    return qp_capi_registry.defs[magic];
}

/* ---- context ------------------------------------------------------------- */

/* The C-API interpreter (exported functions take no context). */
static inline JSContext *qp_capi_cx(void)
{
    return qp_capi.ctx;
}

/* Raise SystemError for a C API call made before qp_capi_init / with a
   NULL argument (PyErr_BadInternalCall). */
static void qp_capi_bad_internal_call(void)
{
    if (qp_capi.ctx)
        PyErr_BadInternalCall();
}

/* Interned atom of a str mirror (JS_ATOM_NULL + exception). */
static JSAtom qp_capi_atom_of(JSContext *ctx, PyObject *name)
{
    JSValue v = qp_capi_value(ctx, name);
    if (JS_IsException(v))
        return JS_ATOM_NULL;
    if (!qp_str_check(ctx, v)) {
        qp_raise_type_error(ctx, "attribute name must be string, not '%T'", v);
        return JS_ATOM_NULL;
    }
    return qp_str_to_atom(ctx, v);
}

/* strdup into process memory (NULL + MemoryError) */
static char *qp_capi_strdup(const char *s)
{
    size_t n = strlen(s) + 1;
    char *d = qp_sys_malloc(n);
    if (!d) {
        PyErr_NoMemory();
        return NULL;
    }
    memcpy(d, s, n);
    return d;
}

/* Fetch / restore the pending exception around code that must not see it
   (dealloc, finalizers). */
static JSValue qp_capi_exc_save(JSContext *ctx)
{
    return JS_HasException(ctx) ? JS_GetException(ctx) : QP_NULL;
}

static void qp_capi_exc_restore(JSContext *ctx, JSValue saved)
{
    if (QP_IS_NULL(saved))
        return;
    if (JS_HasException(ctx))
        JS_FreeValue(ctx, JS_GetException(ctx));
    JS_Throw(ctx, saved);
}

/* ========================================================================= */
/* Slot iterator (CPython 3.15 Python/slots.c)                               */
/* ========================================================================= */

typedef enum QPCapiSlotDType {
    QP_CAPI_SLOT_DTYPE_VOID,
    QP_CAPI_SLOT_DTYPE_FUNC,
    QP_CAPI_SLOT_DTYPE_PTR,
    QP_CAPI_SLOT_DTYPE_SIZE,
    QP_CAPI_SLOT_DTYPE_INT64,
    QP_CAPI_SLOT_DTYPE_UINT64,
} QPCapiSlotDType;

typedef enum QPCapiSlotKind {
    QP_CAPI_SLOT_KIND_TYPE,
    QP_CAPI_SLOT_KIND_MOD,
    QP_CAPI_SLOT_KIND_COMPAT,
    QP_CAPI_SLOT_KIND_SLOT,
} QPCapiSlotKind;

typedef enum QPCapiSlotProblem {
    QP_CAPI_SLOT_PROBLEM_ALLOW,
    QP_CAPI_SLOT_PROBLEM_DEPRECATED,
    QP_CAPI_SLOT_PROBLEM_REJECT,
} QPCapiSlotProblem;

static void qp_capi_slot_err_bad_slot(const char *kind, uint16_t id);

#include "capi_type_slots.h"

static const char *qp_capi_slot_get_name(uint16_t id)
{
    if (id >= _Py_slot_COUNT)
        return "<unknown_slot>";
    if (id == Py_slot_invalid)
        return "Py_slot_invalid";
    return qp_capi_slot_names[id];
}

static void qp_capi_slot_err_bad_slot(const char *kind, uint16_t id)
{
    if (id < _Py_slot_COUNT)
        PyErr_Format(PyExc_SystemError, "invalid %s slot %d (%s)",
                     kind, (unsigned int)id, qp_capi_slot_names[id]);
    else if (id == Py_slot_invalid)
        PyErr_Format(PyExc_SystemError, "invalid slot (Py_slot_invalid, %u)",
                     (unsigned int)id);
    else
        PyErr_Format(PyExc_SystemError, "unknown %s slot ID %u", kind, (unsigned int)id);
}

#define QP_CAPI_SLOT_MAX_NESTING 5
#define QP_CAPI_SLOT_SEEN_BITS (8 * sizeof(unsigned int))

typedef struct QPCapiSlotIterState {
    union {
        const PySlot *slot;
        const PyType_Slot *tp_slot;
        const PyModuleDef_Slot *mod_slot;
        const void *any_slot;
    } u;
    QPCapiSlotKind slot_struct_kind;
} QPCapiSlotIterState;

typedef struct QPCapiSlotIter {
    QPCapiSlotIterState *state;
    QPCapiSlotIterState states[QP_CAPI_SLOT_MAX_NESTING];
    unsigned int seen[_Py_slot_COUNT / QP_CAPI_SLOT_SEEN_BITS + 1];
    QPCapiSlotKind kind;
    uint8_t recursion_level;
    bool is_at_end;
    bool is_first_run;
    const char *name;           /* name of the type/module, NULL if unknown */
    PySlot current;             /* output: copy of the current slot */
} QPCapiSlotIter;

static const char *qp_capi_slot_kind_name(QPCapiSlotKind kind)
{
    switch (kind) {
    case QP_CAPI_SLOT_KIND_TYPE: return "type";
    case QP_CAPI_SLOT_KIND_MOD: return "module";
    case QP_CAPI_SLOT_KIND_COMPAT: return "compat";
    default: return "generic slot";
    }
}

static void qp_capi_slotiter_init_kind(QPCapiSlotIter *it, const void *slots,
                                       QPCapiSlotKind result_kind, QPCapiSlotKind struct_kind)
{
    it->state = it->states;
    it->state->u.any_slot = slots;
    it->state->slot_struct_kind = struct_kind;
    it->kind = result_kind;
    it->name = NULL;
    it->recursion_level = 0;
    it->is_at_end = false;
    it->is_first_run = true;
    it->current.sl_id = 0;
    memset(it->seen, 0, sizeof(it->seen));
}

/* _PySlotIterator_Init (PySlot array) */
static void qp_capi_slotiter_init(QPCapiSlotIter *it, const PySlot *slots, QPCapiSlotKind kind)
{
    qp_capi_slotiter_init_kind(it, slots, kind, QP_CAPI_SLOT_KIND_SLOT);
}

/* _PySlotIterator_InitLegacy (PyType_Slot / PyModuleDef_Slot array) */
static void qp_capi_slotiter_init_legacy(QPCapiSlotIter *it, const void *slots, QPCapiSlotKind kind)
{
    qp_capi_slotiter_init_kind(it, slots, kind, kind);
}

/* _PySlotIterator_Rewind (successfully exhausted iterator, same slots) */
static void qp_capi_slotiter_rewind(QPCapiSlotIter *it, const void *slots)
{
    QPCapiSlotKind k = it->states[0].slot_struct_kind;
    it->is_at_end = false;
    it->state = it->states;
    it->recursion_level = 0;
    it->state->u.any_slot = slots;
    it->state->slot_struct_kind = k;
    it->is_first_run = false;
}

static bool qp_capi_slotiter_saw(QPCapiSlotIter *it, int id)
{
    return (it->seen[id / QP_CAPI_SLOT_SEEN_BITS] >> (id % QP_CAPI_SLOT_SEEN_BITS)) & 1;
}

static void qp_capi_slotiter_advance(QPCapiSlotIter *it)
{
    switch (it->state->slot_struct_kind) {
    case QP_CAPI_SLOT_KIND_SLOT: it->state->u.slot++; break;
    case QP_CAPI_SLOT_KIND_TYPE: it->state->u.tp_slot++; break;
    case QP_CAPI_SLOT_KIND_MOD: it->state->u.mod_slot++; break;
    default: break;
    }
}

/* handle_first_run: validate the current slot, bookkeeping */
static int qp_capi_slotiter_first_run(QPCapiSlotIter *it)
{
    int id = it->current.sl_id;
    QPCapiSlotProblem h;

    if (qp_capi_slot_get_must_be_static(id) && !(it->current.sl_flags & PySlot_STATIC) &&
        it->state->slot_struct_kind == QP_CAPI_SLOT_KIND_SLOT) {
        PyErr_Format(PyExc_SystemError, "%s requires PySlot_STATIC", qp_capi_slot_get_name(id));
        return -1;
    }
    h = qp_capi_slot_get_null_handling(id);
    if (h != QP_CAPI_SLOT_PROBLEM_ALLOW) {
        bool is_null = false;
        switch (qp_capi_slot_get_dtype(id)) {
        case QP_CAPI_SLOT_DTYPE_PTR: is_null = it->current.sl_ptr == NULL; break;
        case QP_CAPI_SLOT_DTYPE_FUNC: is_null = it->current.sl_func == NULL; break;
        default: break;
        }
        if (is_null) {
            if (h == QP_CAPI_SLOT_PROBLEM_REJECT) {
                PyErr_Format(PyExc_SystemError, "NULL not allowed for slot %s",
                             qp_capi_slot_get_name(id));
                return -1;
            }
            if (it->states[0].slot_struct_kind == QP_CAPI_SLOT_KIND_SLOT &&
                PyErr_WarnFormat(PyExc_DeprecationWarning, 1,
                                 "NULL value in slot %s is deprecated",
                                 qp_capi_slot_get_name(id)) < 0)
                return -1;
        }
    }
    h = qp_capi_slot_get_duplicate_handling(id);
    if (h != QP_CAPI_SLOT_PROBLEM_ALLOW && qp_capi_slotiter_saw(it, id)) {
        if (h == QP_CAPI_SLOT_PROBLEM_REJECT) {
            PyErr_Format(PyExc_SystemError, "%s%s%s has multiple %s (%d) slots",
                         qp_capi_slot_kind_name(it->kind), it->name ? " " : "",
                         it->name ? it->name : "", qp_capi_slot_get_name(id),
                         (int)it->current.sl_id);
            return -1;
        }
        if (it->states[0].slot_struct_kind == QP_CAPI_SLOT_KIND_SLOT &&
            PyErr_WarnFormat(PyExc_DeprecationWarning, 0,
                             "%s%s%s has multiple %s (%d) slots. This is deprecated.",
                             qp_capi_slot_kind_name(it->kind), it->name ? " " : "",
                             it->name ? it->name : "", qp_capi_slot_get_name(id),
                             (int)it->current.sl_id) < 0)
            return -1;
    }
    it->seen[id / QP_CAPI_SLOT_SEEN_BITS] |= 1u << (id % QP_CAPI_SLOT_SEEN_BITS);
    return 0;
}

/* _PySlotIterator_Next: false at the end; true otherwise, with
   current.sl_id == Py_slot_invalid and an exception on error. */
static bool qp_capi_slotiter_next(QPCapiSlotIter *it)
{
    it->current.sl_id = (uint16_t)-1;
    for (;;) {
        PySlot *res = &it->current;
        uint16_t flags, orig_id;

        if (it->state->u.any_slot == NULL) {
            if (it->recursion_level == 0) {
                it->is_at_end = true;
                return false;
            }
            it->recursion_level--;
            it->state = &it->states[it->recursion_level];
            qp_capi_slotiter_advance(it);
            continue;
        }
        switch (it->state->slot_struct_kind) {
        case QP_CAPI_SLOT_KIND_SLOT:
            *res = *it->state->u.slot;
            break;
        case QP_CAPI_SLOT_KIND_TYPE:
            memset(res, 0, sizeof(*res));
            res->sl_id = (uint16_t)it->state->u.tp_slot->slot;
            res->sl_flags = PySlot_INTPTR;
            res->sl_ptr = it->state->u.tp_slot->pfunc;
            break;
        case QP_CAPI_SLOT_KIND_MOD:
            memset(res, 0, sizeof(*res));
            res->sl_id = (uint16_t)it->state->u.mod_slot->slot;
            res->sl_flags = PySlot_INTPTR;
            res->sl_ptr = it->state->u.mod_slot->value;
            break;
        default:
            break;
        }
        flags = res->sl_flags;
        orig_id = res->sl_id;
        res->sl_id = it->kind == QP_CAPI_SLOT_KIND_TYPE
            ? qp_capi_slot_resolve_type_slot(res->sl_id)
            : qp_capi_slot_resolve_mod_slot(res->sl_id);
        if (res->sl_id == Py_slot_invalid) {
            if (flags & PySlot_OPTIONAL) {
                qp_capi_slotiter_advance(it);
                continue;
            }
            qp_capi_slot_err_bad_slot(qp_capi_slot_kind_name(it->kind), orig_id);
            goto error;
        }
        if (res->sl_id == Py_slot_end) {
            if (flags & PySlot_OPTIONAL) {
                PyErr_Format(PyExc_SystemError, "invalid flags for Py_slot_end: 0x%x",
                             (unsigned int)flags);
                goto error;
            }
            it->state->u.any_slot = NULL;
            continue;
        }
        if (res->sl_id == Py_slot_subslots || res->sl_id == Py_tp_slots ||
            res->sl_id == Py_mod_slots) {
            uint16_t sub = res->sl_id;
            if (res->sl_ptr == NULL) {
                qp_capi_slotiter_advance(it);
                continue;
            }
            if (it->states[0].slot_struct_kind == QP_CAPI_SLOT_KIND_MOD &&
                it->state->slot_struct_kind == QP_CAPI_SLOT_KIND_SLOT &&
                !(res->sl_flags & PySlot_STATIC)) {
                PyErr_Format(PyExc_SystemError, "slots included from PyModuleDef must be static");
                goto error;
            }
            it->recursion_level++;
            if (it->recursion_level >= QP_CAPI_SLOT_MAX_NESTING) {
                PyErr_Format(PyExc_SystemError, "%s (slot %d): too many levels of nested slots",
                             qp_capi_slot_get_name(sub), orig_id);
                goto error;
            }
            it->state = &it->states[it->recursion_level];
            memset(it->state, 0, sizeof(*it->state));
            it->state->u.any_slot = res->sl_ptr;
            it->state->slot_struct_kind = sub == Py_slot_subslots ? QP_CAPI_SLOT_KIND_SLOT
                : sub == Py_tp_slots ? QP_CAPI_SLOT_KIND_TYPE : QP_CAPI_SLOT_KIND_MOD;
            continue;
        }
        if (flags & PySlot_INTPTR) {
            switch (qp_capi_slot_get_dtype(res->sl_id)) {
            case QP_CAPI_SLOT_DTYPE_SIZE: res->sl_size = (Py_ssize_t)(intptr_t)res->sl_ptr; break;
            case QP_CAPI_SLOT_DTYPE_INT64: res->sl_int64 = (int64_t)(intptr_t)res->sl_ptr; break;
            case QP_CAPI_SLOT_DTYPE_UINT64: res->sl_uint64 = (uint64_t)(intptr_t)res->sl_ptr; break;
            default: break;
            }
        }
        qp_capi_slotiter_advance(it);
        if (it->is_first_run && qp_capi_slotiter_first_run(it) < 0)
            goto error;
        return res->sl_id != Py_slot_end;
    }
error:
    it->current.sl_id = Py_slot_invalid;
    return true;
}

/* ========================================================================= */
/* Result checks and argument arrays                                          */
/* ========================================================================= */

/* Internal API implemented by E1 (capi/compat/pycore_pyerrors.h). */
PyAPI_FUNC(PyObject *) _PyErr_FormatFromCause(PyObject *exception, const char *format, ...);

/* _Py_CheckFunctionResult for a C function named `name`: NULL result
   without error / result with error -> SystemError.  Consumes res. */
static PyObject *qp_capi_check_c_result(JSContext *ctx, PyObject *res, const char *name)
{
    if (!res) {
        if (!JS_HasException(ctx))
            PyErr_Format(PyExc_SystemError, "%s returned NULL without setting an exception",
                         name ? name : "<C function>");
        return NULL;
    }
    if (JS_HasException(ctx)) {
        Py_DECREF(res);
        _PyErr_FormatFromCause(PyExc_SystemError, "%s returned a result with an exception set",
                               name ? name : "<C function>");
        return NULL;
    }
    return res;
}

/* C result -> JS (consumes res): JS_EXCEPTION when NULL. */
static JSValue qp_capi_result_to_js(JSContext *ctx, PyObject *res, const char *name)
{
    JSValue v;
    res = qp_capi_check_c_result(ctx, res, name);
    if (!res)
        return JS_EXCEPTION;
    v = qp_capi_to_js(ctx, res);
    Py_DECREF(res);
    return v;
}

/* Vectorcall argument block: v[0] is scratch (PY_VECTORCALL_ARGUMENTS_OFFSET),
   v[1..n] new references. */
typedef struct QPCapiArgs {
    PyObject *small[9];
    PyObject **v;
    int n;
} QPCapiArgs;

static int qp_capi_args_init(JSContext *ctx, QPCapiArgs *a, JSValueConst *args, int n)
{
    int i;
    a->n = 0;
    a->v = n + 1 <= (int)countof(a->small) ? a->small : js_malloc(ctx, (n + 1) * sizeof(PyObject *));
    if (!a->v)
        return -1;
    a->v[0] = NULL;
    for (i = 0; i < n; i++) {
        PyObject *o = qp_capi_new_ref(ctx, args[i]);
        if (!o) {
            a->n = i;
            return -1;
        }
        a->v[i + 1] = o;
    }
    a->n = n;
    return 0;
}

static void qp_capi_args_free(JSContext *ctx, QPCapiArgs *a)
{
    int i;
    if (!a->v)
        return;
    for (i = 1; i <= a->n; i++)
        Py_DECREF(a->v[i]);
    if (a->v != a->small)
        js_free(ctx, a->v);
    a->v = NULL;
}

/* {kwnames[i]: values[i]} dict mirror (new ref), NULL if no keywords. */
static PyObject *qp_capi_kwargs_dict(PyObject *kwnames, PyObject *const *values, int *err)
{
    PyObject *d;
    Py_ssize_t i, n;
    *err = 0;
    if (!kwnames || (n = PyTuple_GET_SIZE(kwnames)) == 0)
        return NULL;
    d = PyDict_New();
    if (!d) {
        *err = 1;
        return NULL;
    }
    for (i = 0; i < n; i++) {
        if (PyDict_SetItem(d, PyTuple_GET_ITEM(kwnames, i), values[i]) < 0) {
            Py_DECREF(d);
            *err = 1;
            return NULL;
        }
    }
    return d;
}

/* ========================================================================= */
/* Method / getset / member registry and call bridges                         */
/* ========================================================================= */

static char qp_capi_def_marker_getset, qp_capi_def_marker_member, qp_capi_def_marker_wrapper;

/* Register a def (dedup on (c_def, key2)); returns the index or -1 (+
   MemoryError). *pnew tells whether the entry is new. */
static int qp_capi_registry_add(const void *c_def, const void *key2, int kind, int *pnew)
{
    struct QPCapiRegistry *r = &qp_capi_registry;
    intptr_t found = (intptr_t)qp_capi_pmap_get(&r->dedup, c_def, key2);
    QPCapiDef *d;

    *pnew = 0;
    if (found)
        return (int)found - 1;
    if (r->count == INT32_MAX) {
        PyErr_NoMemory();
        return -1;
    }
    if (r->count >= r->size) {
        int ns = r->size ? r->size * 2 : 64;
        QPCapiDef **nd = qp_sys_realloc(r->defs, ns * sizeof(*nd));
        if (!nd) {
            PyErr_NoMemory();
            return -1;
        }
        r->defs = nd;
        r->size = ns;
    }
    d = qp_sys_calloc(1, sizeof(*d));
    if (!d) {
        PyErr_NoMemory();
        return -1;
    }
    d->kind = kind;
    if (qp_capi_pmap_set(&r->dedup, c_def, key2, (void *)(intptr_t)(r->count + 1)) < 0) {
        qp_sys_free(d);
        PyErr_NoMemory();
        return -1;
    }
    r->defs[r->count] = d;
    *pnew = 1;
    return r->count++;
}

static JSValue qp_capi_meth_call(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                 int nargs, JSValueConst kwnames, int magic);
static JSValue qp_capi_getset_get(JSContext *ctx, JSValueConst self, int magic);
static int qp_capi_getset_set(JSContext *ctx, JSValueConst self, JSValueConst value, int magic);
static JSValue qp_capi_member_get(JSContext *ctx, JSValueConst self, int magic);
static int qp_capi_member_set(JSContext *ctx, JSValueConst self, JSValueConst value, int magic);

/* METH_* -> QP_METH_* (argument-count checks with CPython's messages are
   made by the runtime; the bridge does the conversions). */
static uint16_t qp_capi_meth_flags(int ml_flags)
{
    uint16_t f;
    switch (ml_flags & (METH_VARARGS | METH_FASTCALL | METH_NOARGS | METH_O |
                        METH_KEYWORDS | METH_METHOD)) {
    case METH_NOARGS: f = QP_METH_NOARGS; break;
    case METH_O: f = QP_METH_O; break;
    case METH_VARARGS: f = QP_METH_FASTCALL | QP_METH_VARARGS; break;
    case METH_FASTCALL: f = QP_METH_FASTCALL; break;
    default: f = QP_METH_FASTCALL | QP_METH_KEYWORDS; break;
    }
    if (ml_flags & METH_CLASS)
        f |= QP_METH_CLASS;
    if (ml_flags & METH_STATIC)
        f |= QP_METH_STATIC;
    if (ml_flags & METH_COEXIST)
        f |= QP_METH_COEXIST;
    return f;
}

static const QPMethodDef *qp_capi_method_def(JSContext *ctx, PyMethodDef *ml,
                                             PyTypeObject *defining_class)
{
    int isnew, idx;
    QPCapiDef *d;
    PyTypeObject *cls = (ml->ml_flags & METH_METHOD) ? defining_class : NULL;

    idx = qp_capi_registry_add(ml, cls, QP_CAPI_DEF_METHOD, &isnew);
    if (idx < 0)
        return NULL;
    d = qp_capi_registry.defs[idx];
    if (isnew) {
        d->u.ml = ml;
        d->cls = cls;
        d->qml.name = ml->ml_name;
        d->qml.meth = qp_capi_meth_call;
        d->qml.flags = qp_capi_meth_flags(ml->ml_flags);
        d->qml.magic = idx;
        d->qml.doc = ml->ml_doc;
    }
    return &d->qml;
}

static const QPGetSetDef *qp_capi_getset_def(JSContext *ctx, PyGetSetDef *gs)
{
    int isnew, idx = qp_capi_registry_add(gs, &qp_capi_def_marker_getset, QP_CAPI_DEF_GETSET, &isnew);
    QPCapiDef *d;
    if (idx < 0)
        return NULL;
    d = qp_capi_registry.defs[idx];
    if (isnew) {
        d->u.gs = gs;
        d->qgs.name = gs->name;
        d->qgs.get = qp_capi_getset_get;
        d->qgs.set = gs->set ? qp_capi_getset_set : NULL;
        d->qgs.magic = idx;
        d->qgs.doc = gs->doc;
    }
    return &d->qgs;
}

static const QPGetSetDef *qp_capi_member_def(JSContext *ctx, PyMemberDef *m)
{
    int isnew, idx = qp_capi_registry_add(m, &qp_capi_def_marker_member, QP_CAPI_DEF_MEMBER, &isnew);
    QPCapiDef *d;
    if (idx < 0)
        return NULL;
    d = qp_capi_registry.defs[idx];
    if (isnew) {
        d->u.m = m;
        d->qgs.name = m->name;
        d->qgs.get = qp_capi_member_get;
        d->qgs.set = (m->flags & Py_READONLY) ? NULL : qp_capi_member_set;
        d->qgs.magic = idx;
        d->qgs.doc = m->doc;
    }
    return &d->qgs;
}

/* PyDescr_NewWrapper entries: called like METH_VARARGS|METH_KEYWORDS */
static const QPMethodDef *qp_capi_wrapper_def(JSContext *ctx, struct wrapperbase *wb, void *wrapped)
{
    int isnew, idx = qp_capi_registry_add(wb, wrapped, QP_CAPI_DEF_WRAPPER, &isnew);
    QPCapiDef *d;
    (void)qp_capi_def_marker_wrapper;
    if (idx < 0)
        return NULL;
    d = qp_capi_registry.defs[idx];
    if (isnew) {
        d->u.wb = wb;
        d->wrapped = wrapped;
        d->qml.name = wb->name;
        d->qml.meth = qp_capi_meth_call;
        d->qml.flags = QP_METH_FASTCALL |
            ((wb->flags & PyWrapperFlag_KEYWORDS) ? QP_METH_KEYWORDS : 0);
        d->qml.magic = idx;
        d->qml.doc = wb->doc;
    }
    return &d->qml;
}

static const char *qp_capi_method_doc(const QPMethodDef *ml)
{
    QPCapiDef *d;
    if (ml->meth == qp_capi_meth_call && (d = qp_capi_def_at(ml->magic)) &&
        d->kind == QP_CAPI_DEF_METHOD)
        return d->u.ml->ml_doc;
    return ml->doc;
}

static const char *qp_capi_getset_doc(const QPGetSetDef *gs)
{
    QPCapiDef *d;
    if ((gs->get == qp_capi_getset_get || gs->get == qp_capi_member_get) &&
        (d = qp_capi_def_at(gs->magic))) {
        if (d->kind == QP_CAPI_DEF_GETSET)
            return d->u.gs->doc;
        if (d->kind == QP_CAPI_DEF_MEMBER)
            return d->u.m->doc;
    }
    return gs->doc;
}

/* Call bridge of every C method (PyMethodDef calling conventions,
   cfunction_call / method_vectorcall_* of CPython). */
static JSValue qp_capi_meth_call(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                 int nargs, JSValueConst kwnames, int magic)
{
    QPCapiDef *d = qp_capi_def_at(magic);
    PyMethodDef *ml;
    PyObject *cself = NULL, *kwn = NULL, *res = NULL, *tup, *kwd;
    QPCapiArgs a;
    QPCapiCall call;
    int nkw = QP_IS_NULL(kwnames) ? 0 : (int)QP_TUPLE_SIZE(kwnames), err;
    const char *name;

    if (!d)
        return qp_raise_system_error(ctx, "bad C method registry index %d", magic);
    if (d->kind == QP_CAPI_DEF_WRAPPER) {
        ml = NULL;
        name = d->u.wb->name;
    } else {
        ml = d->u.ml;
        name = ml->ml_name;
    }
    if (!QP_IS_NULL(self) && !(ml && (ml->ml_flags & METH_STATIC))) {
        cself = qp_capi_new_ref(ctx, self);
        if (!cself)
            return JS_EXCEPTION;
    }
    if (qp_capi_args_init(ctx, &a, args, nargs + nkw) < 0)
        goto done;
    if (nkw) {
        kwn = qp_capi_new_ref(ctx, kwnames);
        if (!kwn)
            goto done;
    }
    if (!ml) {
        /* wrapper_descriptor semantics: wrapper(self, args, wrapped[, kwds]) */
        tup = PyTuple_FromArray(a.v + 1, nargs);
        if (!tup)
            goto done;
        kwd = qp_capi_kwargs_dict(kwn, a.v + 1 + nargs, &err);
        if (!err) {
            if (d->u.wb->flags & PyWrapperFlag_KEYWORDS)
                res = ((wrapperfunc_kwds)(void (*)(void))d->u.wb->wrapper)(cself, tup, d->wrapped, kwd);
            else
                res = d->u.wb->wrapper(cself, tup, d->wrapped);
        }
        Py_XDECREF(kwd);
        Py_DECREF(tup);
        goto done;
    }
    qp_capi_call_enter(ctx, &call, cself, a.v + 1, self, args, nargs + nkw);
    switch (ml->ml_flags & (METH_VARARGS | METH_FASTCALL | METH_NOARGS | METH_O |
                            METH_KEYWORDS | METH_METHOD)) {
    case METH_NOARGS:
        res = ml->ml_meth(cself, NULL);
        break;
    case METH_O:
        res = ml->ml_meth(cself, a.v[1]);
        break;
    case METH_VARARGS:
        tup = PyTuple_FromArray(a.v + 1, nargs);
        if (!tup)
            goto done;
        res = ml->ml_meth(cself, tup);
        Py_DECREF(tup);
        break;
    case METH_VARARGS | METH_KEYWORDS:
        tup = PyTuple_FromArray(a.v + 1, nargs);
        if (!tup)
            goto done;
        kwd = qp_capi_kwargs_dict(kwn, a.v + 1 + nargs, &err);
        if (!err)
            res = _PyCFunctionWithKeywords_CAST(ml->ml_meth)(cself, tup, kwd);
        Py_XDECREF(kwd);
        Py_DECREF(tup);
        break;
    case METH_FASTCALL:
        res = _PyCFunctionFast_CAST(ml->ml_meth)(cself, a.v + 1, nargs);
        break;
    case METH_FASTCALL | METH_KEYWORDS:
        /* METH_FASTCALL functions get the plain count (CPython's
           cfunction_vectorcall_FASTCALL_KEYWORDS masks nargsf) */
        res = _PyCFunctionFastWithKeywords_CAST(ml->ml_meth)(cself, a.v + 1, nargs, kwn);
        break;
    case METH_METHOD | METH_FASTCALL | METH_KEYWORDS:
        res = ((PyCMethod)(void (*)(void))ml->ml_meth)(cself, d->cls, a.v + 1, nargs, kwn);
        break;
    default:
        qp_capi_call_leave(&call);
        PyErr_Format(PyExc_SystemError, "%s() method: bad call flags", ml->ml_name);
        goto done;
    }
    qp_capi_call_leave(&call);
    qp_capi_args_free(ctx, &a);
    Py_XDECREF(kwn);
    Py_XDECREF(cself);
    return qp_capi_result_to_js(ctx, res, name);
done:
    qp_capi_args_free(ctx, &a);
    Py_XDECREF(kwn);
    Py_XDECREF(cself);
    if (res)
        return qp_capi_result_to_js(ctx, res, name);
    if (!JS_HasException(ctx))
        PyErr_Format(PyExc_SystemError, "%s returned NULL without setting an exception", name);
    return JS_EXCEPTION;
}

static JSValue qp_capi_getset_get(JSContext *ctx, JSValueConst self, int magic)
{
    QPCapiDef *d = qp_capi_def_at(magic);
    PyObject *x, *res;
    if (!d || d->kind != QP_CAPI_DEF_GETSET)
        return qp_raise_system_error(ctx, "bad C getset registry index %d", magic);
    if (!d->u.gs->get)
        return qp_raise(ctx, QP_EXC(ctx, AttributeError), "attribute '%s' is not readable",
                        d->u.gs->name);
    x = qp_capi_new_ref(ctx, self);
    if (!x)
        return JS_EXCEPTION;
    res = d->u.gs->get(x, d->u.gs->closure);
    Py_DECREF(x);
    return qp_capi_result_to_js(ctx, res, d->u.gs->name);
}

static int qp_capi_getset_set(JSContext *ctx, JSValueConst self, JSValueConst value, int magic)
{
    QPCapiDef *d = qp_capi_def_at(magic);
    PyObject *x, *v = NULL;
    int r;
    if (!d || d->kind != QP_CAPI_DEF_GETSET) {
        qp_raise_system_error(ctx, "bad C getset registry index %d", magic);
        return -1;
    }
    x = qp_capi_new_ref(ctx, self);
    if (!x)
        return -1;
    if (!QP_IS_NULL(value)) {
        v = qp_capi_new_ref(ctx, value);
        if (!v) {
            Py_DECREF(x);
            return -1;
        }
    }
    r = d->u.gs->set(x, v, d->u.gs->closure);
    Py_XDECREF(v);
    Py_DECREF(x);
    if (r < 0 && !JS_HasException(ctx))
        PyErr_Format(PyExc_SystemError, "setter of '%s' failed without setting an exception",
                     d->u.gs->name);
    return r < 0 ? -1 : 0;
}

static JSValue qp_capi_member_get(JSContext *ctx, JSValueConst self, int magic)
{
    QPCapiDef *d = qp_capi_def_at(magic);
    PyObject *x, *res;
    if (!d || d->kind != QP_CAPI_DEF_MEMBER)
        return qp_raise_system_error(ctx, "bad C member registry index %d", magic);
    x = qp_capi_new_ref(ctx, self);
    if (!x)
        return JS_EXCEPTION;
    if ((d->u.m->flags & Py_AUDIT_READ) &&
        PySys_Audit("object.__getattr__", "Os", x, d->u.m->name) < 0) {
        Py_DECREF(x);
        return JS_EXCEPTION;
    }
    if ((d->u.m->type == _Py_T_OBJECT || d->u.m->type == Py_T_OBJECT_EX) &&
        Py_TYPE(x)->tp_weaklistoffset > 0 && d->u.m->offset == Py_TYPE(x)->tp_weaklistoffset) {
        /* the weak reference list lives on J (QuickJS weak references):
           its head is the first weak reference, as __weakref__ */
        Py_DECREF(x);
        return qp_weakref_get_first(ctx, self);
    }
    res = PyMember_GetOne((const char *)x, d->u.m);
    Py_DECREF(x);
    if (!res)
        return JS_EXCEPTION;
    return qp_capi_result_to_js(ctx, res, d->u.m->name);
}

static int qp_capi_member_set(JSContext *ctx, JSValueConst self, JSValueConst value, int magic)
{
    QPCapiDef *d = qp_capi_def_at(magic);
    PyObject *x, *v = NULL;
    int r;
    if (!d || d->kind != QP_CAPI_DEF_MEMBER) {
        qp_raise_system_error(ctx, "bad C member registry index %d", magic);
        return -1;
    }
    x = qp_capi_new_ref(ctx, self);
    if (!x)
        return -1;
    if (!QP_IS_NULL(value)) {
        v = qp_capi_new_ref(ctx, value);
        if (!v) {
            Py_DECREF(x);
            return -1;
        }
    }
    r = PyMember_SetOne((char *)x, d->u.m, v);
    Py_XDECREF(v);
    Py_DECREF(x);
    return r < 0 ? -1 : 0;
}

/* ========================================================================= */
/* Argument conversions between the C and QP call conventions                 */
/* ========================================================================= */

/* QP vectorcall arguments built from a C (args tuple, kwargs dict) pair. */
typedef struct QPCapiVC {
    JSValue small[8];
    JSValue *argv;              /* nargs positionals then nkw keyword values (owned) */
    int nargs, nkw;
    JSValue kwnames;            /* tuple of str or QP_NULL (owned) */
} QPCapiVC;

static void qp_capi_vc_free(JSContext *ctx, QPCapiVC *vc)
{
    int i;
    if (vc->argv) {
        for (i = 0; i < vc->nargs + vc->nkw; i++)
            JS_FreeValue(ctx, vc->argv[i]);
        if (vc->argv != vc->small)
            js_free(ctx, vc->argv);
        vc->argv = NULL;
    }
    JS_FreeValue(ctx, vc->kwnames);
    vc->kwnames = QP_NULL;
}

static int qp_capi_vc_from_c(JSContext *ctx, PyObject *args, PyObject *kwds, QPCapiVC *vc)
{
    JSValue tj = QP_NULL, dj = QP_NULL, key, val;
    JSValue *names = NULL;
    Py_ssize_t pos = 0;
    int i, n = 0, nkw = 0;

    vc->argv = NULL;
    vc->nargs = vc->nkw = 0;
    vc->kwnames = QP_NULL;
    if (args) {
        tj = qp_capi_value(ctx, args);
        if (JS_IsException(tj))
            return -1;
        if (!qp_tuple_check(ctx, tj)) {
            qp_raise_system_error(ctx, "argument list must be a tuple, not '%T'", tj);
            return -1;
        }
        n = (int)QP_TUPLE_SIZE(tj);
    }
    if (kwds) {
        dj = qp_capi_value(ctx, kwds);
        if (JS_IsException(dj))
            return -1;
        if (!qp_dict_check(ctx, dj)) {
            qp_raise_system_error(ctx, "keyword arguments must be a dict, not '%T'", dj);
            return -1;
        }
        nkw = (int)qp_dict_size(dj);
    }
    vc->argv = n + nkw <= (int)countof(vc->small) ? vc->small
                                                  : js_malloc(ctx, (n + nkw) * sizeof(JSValue));
    if (!vc->argv)
        return -1;
    for (i = 0; i < n; i++)
        vc->argv[i] = JS_DupValue(ctx, QP_TUPLE_ITEMS(tj)[i]);
    vc->nargs = n;
    if (nkw) {
        names = js_malloc(ctx, nkw * sizeof(JSValue));
        if (!names)
            return -1;
        i = 0;
        /* qp_dict_next: borrowed key and value */
        while (i < nkw && qp_dict_next(dj, &pos, &key, &val)) {
            if (!qp_str_check(ctx, key)) {
                while (i-- > 0)
                    JS_FreeValue(ctx, names[i]);
                js_free(ctx, names);
                qp_raise_type_error(ctx, "keywords must be strings");
                return -1;
            }
            names[i] = JS_DupValue(ctx, key);
            vc->argv[n + i] = JS_DupValue(ctx, val);
            vc->nkw = ++i;
        }
        vc->kwnames = qp_tuple_new_steal(ctx, vc->nkw, names);
        js_free(ctx, names);
        if (JS_IsException(vc->kwnames)) {
            vc->kwnames = QP_NULL;
            return -1;
        }
    }
    return 0;
}

/* C (tuple, dict-or-NULL) new mirrors of QP vectorcall arguments. */
static int qp_capi_vc_to_c(JSContext *ctx, JSValueConst *args, int nargs, JSValueConst kwnames,
                           PyObject **ptup, PyObject **pkw)
{
    JSValue t, d;
    int i, nkw = QP_IS_NULL(kwnames) ? 0 : (int)QP_TUPLE_SIZE(kwnames);

    *ptup = *pkw = NULL;
    t = qp_tuple_pack(ctx, nargs, args);
    if (JS_IsException(t))
        return -1;
    *ptup = qp_capi_steal(ctx, t);
    if (!*ptup)
        return -1;
    if (nkw) {
        d = qp_dict_new(ctx);
        if (JS_IsException(d))
            goto fail;
        for (i = 0; i < nkw; i++) {
            if (qp_dict_set_item(ctx, d, QP_TUPLE_ITEMS(kwnames)[i], args[nargs + i]) < 0) {
                JS_FreeValue(ctx, d);
                goto fail;
            }
        }
        *pkw = qp_capi_steal(ctx, d);
        if (!*pkw)
            goto fail;
    }
    return 0;
fail:
    Py_CLEAR(*ptup);
    return -1;
}

/* JS result of a QP slot -> C (consumes r).  QP_NULL -> NULL without an
   error (iternext exhaustion). */
static PyObject *qp_capi_from_qp(JSContext *ctx, JSValue r)
{
    if (QP_IS_NULL(r))
        return NULL;
    return qp_capi_from_js_result(ctx, r);
}

/* ========================================================================= */
/* Forward-bridge registry (needed by the reverse bridges to skip them)       */
/* ========================================================================= */

static QPCapiPMap qp_capi_fwd_set;      /* forward bridge function -> 1 */

static inline int qp_capi_is_fwd(const void *fn)
{
    return fn && qp_capi_pmap_get(&qp_capi_fwd_set, fn, NULL) != NULL;
}

#define QP_CAPI_QSLOT(t, off) (*(void **)((uint8_t *)&(t)->slots + (off)))

/* Nearest implementation of QP slot `off` for objects of type t that is
   not a forward bridge (the C function behind a forward bridge is reached
   directly by C code): NULL when t does not implement the slot. */
static void *qp_capi_qslot_nearest(QPType *t, size_t off)
{
    void *f = QP_CAPI_QSLOT(t, off);
    Py_ssize_t i, n;
    JSValue *items;

    if (!qp_capi_is_fwd(f))
        return f;
    if (JS_VALUE_GET_TAG(t->mro) == JS_TAG_OBJECT) {
        n = QP_TUPLE_SIZE(t->mro);
        items = QP_TUPLE_ITEMS(t->mro);
        for (i = 1; i < n; i++) {
            QPType *u = qp_type_from_value(items[i]);
            if (!u)
                continue;
            f = QP_CAPI_QSLOT(u, off);
            if (!f)
                return NULL;
            if (!qp_capi_is_fwd(f))
                return f;
        }
        return NULL;
    }
    for (t = t->base; t; t = t->base) {
        f = QP_CAPI_QSLOT(t, off);
        if (!f)
            return NULL;
        if (!qp_capi_is_fwd(f))
            return f;
    }
    return NULL;
}

#define QP_CAPI_QOFF(F) offsetof(QPTypeSlots, F)

/* ========================================================================= */
/* Reverse bridges: C slot call -> QP slot                                    */
/* ========================================================================= */

/* t != NULL: that type's slot (dedicated bridges of static types);
   t == NULL: the nearest non-forward implementation of the object's type. */
#define QP_CAPI_RB_PROLOGUE(o, v, errval)                                   \
    JSContext *ctx = qp_capi.ctx;                                           \
    JSValue v;                                                              \
    if (!ctx) { qp_capi_bad_internal_call(); return errval; }               \
    v = qp_capi_value(ctx, o);                                              \
    if (JS_IsException(v)) return errval

static PyObject *qp_capi_rb_new_impl(QPType *t, PyTypeObject *subtype, PyObject *args,
                                     PyObject *kwds)
{
    JSContext *ctx = qp_capi.ctx;
    QPType *sub;
    QPNewFunc *f;
    QPCapiVC vc;
    JSValue r;

    if (!ctx || !subtype) {
        qp_capi_bad_internal_call();
        return NULL;
    }
    sub = qp_capi_type_qp(subtype);
    if (!sub)
        return NULL;
    f = t ? t->slots.tp_new : (QPNewFunc *)qp_capi_qslot_nearest(sub, QP_CAPI_QOFF(tp_new));
    if (!f) {
        PyErr_Format(PyExc_TypeError, "cannot create '%s' instances", subtype->tp_name);
        return NULL;
    }
    if (qp_capi_vc_from_c(ctx, args, kwds, &vc) < 0) {
        qp_capi_vc_free(ctx, &vc);
        return NULL;
    }
    r = f(ctx, sub, (JSValueConst *)vc.argv, vc.nargs, vc.kwnames);
    qp_capi_vc_free(ctx, &vc);
    return qp_capi_from_js_result(ctx, r);
}

static int qp_capi_rb_init_impl(QPType *t, PyObject *o, PyObject *args, PyObject *kwds)
{
    QPInitProc *f;
    QPCapiVC vc;
    int r;
    QP_CAPI_RB_PROLOGUE(o, v, -1);
    f = t ? t->slots.tp_init
          : (QPInitProc *)qp_capi_qslot_nearest(qp_type_of(ctx, v), QP_CAPI_QOFF(tp_init));
    if (!f)
        return 0;
    if (qp_capi_vc_from_c(ctx, args, kwds, &vc) < 0) {
        qp_capi_vc_free(ctx, &vc);
        return -1;
    }
    r = f(ctx, v, (JSValueConst *)vc.argv, vc.nargs, vc.kwnames);
    qp_capi_vc_free(ctx, &vc);
    return r < 0 ? -1 : 0;
}

static PyObject *qp_capi_rb_repr_impl(QPType *t, PyObject *o)
{
    QPUnaryFunc *f;
    QP_CAPI_RB_PROLOGUE(o, v, NULL);
    f = t ? t->slots.tp_repr
          : (QPUnaryFunc *)qp_capi_qslot_nearest(qp_type_of(ctx, v), QP_CAPI_QOFF(tp_repr));
    return qp_capi_from_js_result(ctx, f ? f(ctx, v) : qp_repr(ctx, v));
}

static PyObject *qp_capi_rb_str_impl(QPType *t, PyObject *o)
{
    QPUnaryFunc *f;
    QP_CAPI_RB_PROLOGUE(o, v, NULL);
    f = t ? t->slots.tp_str
          : (QPUnaryFunc *)qp_capi_qslot_nearest(qp_type_of(ctx, v), QP_CAPI_QOFF(tp_str));
    return qp_capi_from_js_result(ctx, f ? f(ctx, v) : qp_str(ctx, v));
}

static Py_hash_t qp_capi_rb_hash_impl(QPType *t, PyObject *o)
{
    QPHashFunc *f;
    QP_CAPI_RB_PROLOGUE(o, v, -1);
    f = t ? t->slots.tp_hash
          : (QPHashFunc *)qp_capi_qslot_nearest(qp_type_of(ctx, v), QP_CAPI_QOFF(tp_hash));
    return f ? f(ctx, v) : qp_hash(ctx, v);
}

static PyObject *qp_capi_rb_richcompare_impl(QPType *t, PyObject *a, PyObject *b, int op)
{
    QPRichCmpFunc *f;
    JSValue vb;
    QP_CAPI_RB_PROLOGUE(a, v, NULL);
    vb = qp_capi_value(ctx, b);
    if (JS_IsException(vb))
        return NULL;
    f = t ? t->slots.tp_richcompare
          : (QPRichCmpFunc *)qp_capi_qslot_nearest(qp_type_of(ctx, v), QP_CAPI_QOFF(tp_richcompare));
    if (!f)
        return qp_capi_from_js_result(ctx, qp_not_implemented(ctx));
    return qp_capi_from_js_result(ctx, f(ctx, v, vb, op));
}

static PyObject *qp_capi_rb_getattro_impl(QPType *t, PyObject *o, PyObject *name)
{
    QPGetAttroFunc *f;
    JSAtom a;
    JSValue r;
    QP_CAPI_RB_PROLOGUE(o, v, NULL);
    a = qp_capi_atom_of(ctx, name);
    if (a == JS_ATOM_NULL)
        return NULL;
    f = t ? t->slots.tp_getattro
          : (QPGetAttroFunc *)qp_capi_qslot_nearest(qp_type_of(ctx, v), QP_CAPI_QOFF(tp_getattro));
    r = f ? f(ctx, v, a) : qp_generic_getattr(ctx, v, a);
    JS_FreeAtom(ctx, a);
    return qp_capi_from_js_result(ctx, r);
}

static int qp_capi_rb_setattro_impl(QPType *t, PyObject *o, PyObject *name, PyObject *value)
{
    QPSetAttroFunc *f;
    JSAtom a;
    JSValue vv = QP_NULL;
    int r;
    QP_CAPI_RB_PROLOGUE(o, v, -1);
    if (value) {
        vv = qp_capi_value(ctx, value);
        if (JS_IsException(vv))
            return -1;
    }
    a = qp_capi_atom_of(ctx, name);
    if (a == JS_ATOM_NULL)
        return -1;
    f = t ? t->slots.tp_setattro
          : (QPSetAttroFunc *)qp_capi_qslot_nearest(qp_type_of(ctx, v), QP_CAPI_QOFF(tp_setattro));
    r = f ? f(ctx, v, a, vv) : qp_generic_setattr(ctx, v, a, vv);
    JS_FreeAtom(ctx, a);
    return r < 0 ? -1 : 0;
}

static PyObject *qp_capi_rb_iter_impl(QPType *t, PyObject *o)
{
    QPUnaryFunc *f;
    QP_CAPI_RB_PROLOGUE(o, v, NULL);
    f = t ? t->slots.tp_iter
          : (QPUnaryFunc *)qp_capi_qslot_nearest(qp_type_of(ctx, v), QP_CAPI_QOFF(tp_iter));
    return qp_capi_from_js_result(ctx, f ? f(ctx, v) : qp_iter(ctx, v));
}

static PyObject *qp_capi_rb_iternext_impl(QPType *t, PyObject *o)
{
    QPIterNextFunc *f;
    QP_CAPI_RB_PROLOGUE(o, v, NULL);
    f = t ? t->slots.tp_iternext
          : (QPIterNextFunc *)qp_capi_qslot_nearest(qp_type_of(ctx, v), QP_CAPI_QOFF(tp_iternext));
    if (!f) {
        PyErr_Format(PyExc_TypeError, "'%.200s' object is not an iterator", Py_TYPE(o)->tp_name);
        return NULL;
    }
    return qp_capi_from_qp(ctx, f(ctx, v));
}

static PyObject *qp_capi_rb_call_impl(QPType *t, PyObject *o, PyObject *args, PyObject *kwds)
{
    QPVectorcallFunc *f;
    QPCapiVC vc;
    JSValue r;
    QP_CAPI_RB_PROLOGUE(o, v, NULL);
    f = t ? t->slots.tp_call
          : (QPVectorcallFunc *)qp_capi_qslot_nearest(qp_type_of(ctx, v), QP_CAPI_QOFF(tp_call));
    if (!f) {
        PyErr_Format(PyExc_TypeError, "'%.200s' object is not callable", Py_TYPE(o)->tp_name);
        return NULL;
    }
    if (qp_capi_vc_from_c(ctx, args, kwds, &vc) < 0) {
        qp_capi_vc_free(ctx, &vc);
        return NULL;
    }
    r = f(ctx, v, (JSValueConst *)vc.argv, vc.nargs, vc.kwnames);
    qp_capi_vc_free(ctx, &vc);
    return qp_capi_from_js_result(ctx, r);
}

static PyObject *qp_capi_rb_descr_get_impl(QPType *t, PyObject *d, PyObject *obj, PyObject *type)
{
    QPDescrGetFunc *f;
    JSValue vo = QP_NULL, vt = QP_NULL;
    QP_CAPI_RB_PROLOGUE(d, v, NULL);
    if (obj && obj != Py_None) {
        vo = qp_capi_value(ctx, obj);
        if (JS_IsException(vo))
            return NULL;
    } else if (obj == Py_None && !type) {
        vo = JS_UNDEFINED;
    }
    if (type) {
        vt = qp_capi_value(ctx, type);
        if (JS_IsException(vt))
            return NULL;
    }
    f = t ? t->slots.tp_descr_get
          : (QPDescrGetFunc *)qp_capi_qslot_nearest(qp_type_of(ctx, v), QP_CAPI_QOFF(tp_descr_get));
    if (!f)
        return Py_NewRef(d);
    return qp_capi_from_js_result(ctx, f(ctx, v, vo, vt));
}

static int qp_capi_rb_descr_set_impl(QPType *t, PyObject *d, PyObject *obj, PyObject *value)
{
    QPDescrSetFunc *f;
    JSValue vo, vv = QP_NULL;
    QP_CAPI_RB_PROLOGUE(d, v, -1);
    vo = qp_capi_value(ctx, obj);
    if (JS_IsException(vo))
        return -1;
    if (value) {
        vv = qp_capi_value(ctx, value);
        if (JS_IsException(vv))
            return -1;
    }
    f = t ? t->slots.tp_descr_set
          : (QPDescrSetFunc *)qp_capi_qslot_nearest(qp_type_of(ctx, v), QP_CAPI_QOFF(tp_descr_set));
    if (!f) {
        PyErr_Format(PyExc_AttributeError, "'%.200s' object is not a data descriptor",
                     Py_TYPE(d)->tp_name);
        return -1;
    }
    return f(ctx, v, vo, vv) < 0 ? -1 : 0;
}

/* ---- generic reverse bridges (dynamic type) ------------------------------ */

static PyObject *qp_capi_rbg_new(PyTypeObject *s, PyObject *a, PyObject *k) { return qp_capi_rb_new_impl(NULL, s, a, k); }
static int qp_capi_rbg_init(PyObject *o, PyObject *a, PyObject *k) { return qp_capi_rb_init_impl(NULL, o, a, k); }
static PyObject *qp_capi_rbg_repr(PyObject *o) { return qp_capi_rb_repr_impl(NULL, o); }
static PyObject *qp_capi_rbg_str(PyObject *o) { return qp_capi_rb_str_impl(NULL, o); }
static Py_hash_t qp_capi_rbg_hash(PyObject *o) { return qp_capi_rb_hash_impl(NULL, o); }
static PyObject *qp_capi_rbg_richcompare(PyObject *a, PyObject *b, int op) { return qp_capi_rb_richcompare_impl(NULL, a, b, op); }
static PyObject *qp_capi_rbg_getattro(PyObject *o, PyObject *n) { return qp_capi_rb_getattro_impl(NULL, o, n); }
static int qp_capi_rbg_setattro(PyObject *o, PyObject *n, PyObject *v) { return qp_capi_rb_setattro_impl(NULL, o, n, v); }
static PyObject *qp_capi_rbg_iter(PyObject *o) { return qp_capi_rb_iter_impl(NULL, o); }
static PyObject *qp_capi_rbg_iternext(PyObject *o) { return qp_capi_rb_iternext_impl(NULL, o); }
static PyObject *qp_capi_rbg_call(PyObject *o, PyObject *a, PyObject *k) { return qp_capi_rb_call_impl(NULL, o, a, k); }
static PyObject *qp_capi_rbg_descr_get(PyObject *d, PyObject *o, PyObject *t) { return qp_capi_rb_descr_get_impl(NULL, d, o, t); }
static int qp_capi_rbg_descr_set(PyObject *d, PyObject *o, PyObject *v) { return qp_capi_rb_descr_set_impl(NULL, d, o, v); }

/* number / sequence / mapping / async suites: always generic */

static PyObject *qp_capi_rb_unary(PyObject *a, size_t off)
{
    QPUnaryFunc *f;
    QP_CAPI_RB_PROLOGUE(a, v, NULL);
    f = (QPUnaryFunc *)qp_capi_qslot_nearest(qp_type_of(ctx, v), off);
    if (!f) {
        PyErr_Format(PyExc_TypeError, "bad operand type for C slot: '%.200s'", Py_TYPE(a)->tp_name);
        return NULL;
    }
    return qp_capi_from_js_result(ctx, f(ctx, v));
}

/* binary: the first operand's implementation, then the second operand's
   when the first returns NotImplemented (C binary slots are called with
   both orders by binary_op1). */
static PyObject *qp_capi_rb_binary(PyObject *a, PyObject *b, size_t off)
{
    QPBinaryFunc *f, *g;
    QPType *ta, *tb;
    JSValue vb, r;
    QP_CAPI_RB_PROLOGUE(a, v, NULL);
    vb = qp_capi_value(ctx, b);
    if (JS_IsException(vb))
        return NULL;
    ta = qp_type_of(ctx, v);
    tb = qp_type_of(ctx, vb);
    f = (QPBinaryFunc *)qp_capi_qslot_nearest(ta, off);
    r = f ? f(ctx, v, vb) : qp_not_implemented(ctx);
    if (!JS_IsException(r) && qp_is_notimpl(ctx, r) && tb != ta) {
        g = (QPBinaryFunc *)qp_capi_qslot_nearest(tb, off);
        if (g && g != f) {
            JS_FreeValue(ctx, r);
            r = g(ctx, v, vb);
        }
    }
    return qp_capi_from_js_result(ctx, r);
}

static PyObject *qp_capi_rb_ternary(PyObject *a, PyObject *b, PyObject *c, size_t off)
{
    QPTernaryFunc *f, *g;
    QPType *ta, *tb;
    JSValue vb, vc, r;
    QP_CAPI_RB_PROLOGUE(a, v, NULL);
    vb = qp_capi_value(ctx, b);
    if (JS_IsException(vb))
        return NULL;
    vc = qp_capi_value(ctx, c ? c : Py_None);
    if (JS_IsException(vc))
        return NULL;
    ta = qp_type_of(ctx, v);
    tb = qp_type_of(ctx, vb);
    f = (QPTernaryFunc *)qp_capi_qslot_nearest(ta, off);
    r = f ? f(ctx, v, vb, vc) : qp_not_implemented(ctx);
    if (!JS_IsException(r) && qp_is_notimpl(ctx, r) && tb != ta) {
        g = (QPTernaryFunc *)qp_capi_qslot_nearest(tb, off);
        if (g && g != f) {
            JS_FreeValue(ctx, r);
            r = g(ctx, v, vb, vc);
        }
    }
    return qp_capi_from_js_result(ctx, r);
}

#define QP_CAPI_RB_UNARY(S, N) \
    static PyObject *qp_capi_rbg_##N(PyObject *a) { return qp_capi_rb_unary(a, QP_CAPI_QOFF(S.N)); }
#define QP_CAPI_RB_BINARY(S, N) \
    static PyObject *qp_capi_rbg_##N(PyObject *a, PyObject *b) { return qp_capi_rb_binary(a, b, QP_CAPI_QOFF(S.N)); }
#define QP_CAPI_RB_TERNARY(S, N) \
    static PyObject *qp_capi_rbg_##N(PyObject *a, PyObject *b, PyObject *c) { return qp_capi_rb_ternary(a, b, c, QP_CAPI_QOFF(S.N)); }

QP_CAPI_RB_BINARY(nb, nb_add) QP_CAPI_RB_BINARY(nb, nb_subtract) QP_CAPI_RB_BINARY(nb, nb_multiply)
QP_CAPI_RB_BINARY(nb, nb_remainder) QP_CAPI_RB_BINARY(nb, nb_divmod)
QP_CAPI_RB_TERNARY(nb, nb_power)
QP_CAPI_RB_UNARY(nb, nb_negative) QP_CAPI_RB_UNARY(nb, nb_positive) QP_CAPI_RB_UNARY(nb, nb_absolute)
QP_CAPI_RB_UNARY(nb, nb_invert)
QP_CAPI_RB_BINARY(nb, nb_lshift) QP_CAPI_RB_BINARY(nb, nb_rshift) QP_CAPI_RB_BINARY(nb, nb_and)
QP_CAPI_RB_BINARY(nb, nb_xor) QP_CAPI_RB_BINARY(nb, nb_or)
QP_CAPI_RB_UNARY(nb, nb_int) QP_CAPI_RB_UNARY(nb, nb_float)
QP_CAPI_RB_BINARY(nb, nb_inplace_add) QP_CAPI_RB_BINARY(nb, nb_inplace_subtract)
QP_CAPI_RB_BINARY(nb, nb_inplace_multiply) QP_CAPI_RB_BINARY(nb, nb_inplace_remainder)
QP_CAPI_RB_TERNARY(nb, nb_inplace_power)
QP_CAPI_RB_BINARY(nb, nb_inplace_lshift) QP_CAPI_RB_BINARY(nb, nb_inplace_rshift)
QP_CAPI_RB_BINARY(nb, nb_inplace_and) QP_CAPI_RB_BINARY(nb, nb_inplace_xor)
QP_CAPI_RB_BINARY(nb, nb_inplace_or)
QP_CAPI_RB_BINARY(nb, nb_floor_divide) QP_CAPI_RB_BINARY(nb, nb_true_divide)
QP_CAPI_RB_BINARY(nb, nb_inplace_floor_divide) QP_CAPI_RB_BINARY(nb, nb_inplace_true_divide)
QP_CAPI_RB_UNARY(nb, nb_index)
QP_CAPI_RB_BINARY(nb, nb_matrix_multiply) QP_CAPI_RB_BINARY(nb, nb_inplace_matrix_multiply)
QP_CAPI_RB_BINARY(sq, sq_concat) QP_CAPI_RB_BINARY(sq, sq_inplace_concat)
QP_CAPI_RB_BINARY(mp, mp_subscript)
QP_CAPI_RB_UNARY(am, am_await) QP_CAPI_RB_UNARY(am, am_aiter) QP_CAPI_RB_UNARY(am, am_anext)

static int qp_capi_rbg_nb_bool(PyObject *a)
{
    QPInquiry *f;
    QP_CAPI_RB_PROLOGUE(a, v, -1);
    f = (QPInquiry *)qp_capi_qslot_nearest(qp_type_of(ctx, v), QP_CAPI_QOFF(nb.nb_bool));
    return f ? f(ctx, v) : qp_bool(ctx, v);
}

static Py_ssize_t qp_capi_rb_len(PyObject *a, size_t off)
{
    QPLenFunc *f;
    QP_CAPI_RB_PROLOGUE(a, v, -1);
    f = (QPLenFunc *)qp_capi_qslot_nearest(qp_type_of(ctx, v), off);
    return f ? f(ctx, v) : qp_len(ctx, v);
}
static Py_ssize_t qp_capi_rbg_sq_length(PyObject *a) { return qp_capi_rb_len(a, QP_CAPI_QOFF(sq.sq_length)); }
static Py_ssize_t qp_capi_rbg_mp_length(PyObject *a) { return qp_capi_rb_len(a, QP_CAPI_QOFF(mp.mp_length)); }

static PyObject *qp_capi_rb_ssizearg(PyObject *a, Py_ssize_t i, size_t off)
{
    QPSsizeArgFunc *f;
    QP_CAPI_RB_PROLOGUE(a, v, NULL);
    f = (QPSsizeArgFunc *)qp_capi_qslot_nearest(qp_type_of(ctx, v), off);
    if (!f) {
        PyErr_Format(PyExc_TypeError, "'%.200s' object does not support indexing",
                     Py_TYPE(a)->tp_name);
        return NULL;
    }
    return qp_capi_from_js_result(ctx, f(ctx, v, i));
}
static PyObject *qp_capi_rbg_sq_repeat(PyObject *a, Py_ssize_t i) { return qp_capi_rb_ssizearg(a, i, QP_CAPI_QOFF(sq.sq_repeat)); }
static PyObject *qp_capi_rbg_sq_item(PyObject *a, Py_ssize_t i) { return qp_capi_rb_ssizearg(a, i, QP_CAPI_QOFF(sq.sq_item)); }
static PyObject *qp_capi_rbg_sq_inplace_repeat(PyObject *a, Py_ssize_t i) { return qp_capi_rb_ssizearg(a, i, QP_CAPI_QOFF(sq.sq_inplace_repeat)); }

static int qp_capi_rbg_sq_ass_item(PyObject *a, Py_ssize_t i, PyObject *value)
{
    QPSsizeObjArgProc *f;
    JSValue vv = QP_NULL;
    QP_CAPI_RB_PROLOGUE(a, v, -1);
    if (value) {
        vv = qp_capi_value(ctx, value);
        if (JS_IsException(vv))
            return -1;
    }
    f = (QPSsizeObjArgProc *)qp_capi_qslot_nearest(qp_type_of(ctx, v), QP_CAPI_QOFF(sq.sq_ass_item));
    if (!f) {
        PyErr_Format(PyExc_TypeError, "'%.200s' object does not support item assignment",
                     Py_TYPE(a)->tp_name);
        return -1;
    }
    return f(ctx, v, i, vv) < 0 ? -1 : 0;
}

static int qp_capi_rbg_sq_contains(PyObject *a, PyObject *b)
{
    QPObjObjProc *f;
    JSValue vb;
    QP_CAPI_RB_PROLOGUE(a, v, -1);
    vb = qp_capi_value(ctx, b);
    if (JS_IsException(vb))
        return -1;
    f = (QPObjObjProc *)qp_capi_qslot_nearest(qp_type_of(ctx, v), QP_CAPI_QOFF(sq.sq_contains));
    return f ? f(ctx, v, vb) : qp_contains(ctx, v, vb);
}

static int qp_capi_rbg_mp_ass_subscript(PyObject *a, PyObject *k, PyObject *value)
{
    QPObjObjArgProc *f;
    JSValue vk, vv = QP_NULL;
    QP_CAPI_RB_PROLOGUE(a, v, -1);
    vk = qp_capi_value(ctx, k);
    if (JS_IsException(vk))
        return -1;
    if (value) {
        vv = qp_capi_value(ctx, value);
        if (JS_IsException(vv))
            return -1;
    }
    f = (QPObjObjArgProc *)qp_capi_qslot_nearest(qp_type_of(ctx, v), QP_CAPI_QOFF(mp.mp_ass_subscript));
    if (!f) {
        PyErr_Format(PyExc_TypeError, "'%.200s' object does not support item assignment",
                     Py_TYPE(a)->tp_name);
        return -1;
    }
    return f(ctx, v, vk, vv) < 0 ? -1 : 0;
}

static PySendResult qp_capi_rbg_am_send(PyObject *it, PyObject *arg, PyObject **presult)
{
    QPSendFunc *f;
    JSValue va, res = QP_NULL;
    int r;
    *presult = NULL;
    {
        QP_CAPI_RB_PROLOGUE(it, v, PYGEN_ERROR);
        va = qp_capi_value(ctx, arg ? arg : Py_None);
        if (JS_IsException(va))
            return PYGEN_ERROR;
        f = (QPSendFunc *)qp_capi_qslot_nearest(qp_type_of(ctx, v), QP_CAPI_QOFF(am.am_send));
        if (!f) {
            PyErr_Format(PyExc_TypeError, "'%.200s' object does not support send",
                         Py_TYPE(it)->tp_name);
            return PYGEN_ERROR;
        }
        r = f(ctx, v, va, &res);
        if (r == QP_SEND_ERROR) {
            JS_FreeValue(ctx, res);
            return PYGEN_ERROR;
        }
        *presult = qp_capi_from_js_result(ctx, QP_IS_NULL(res) ? JS_UNDEFINED : res);
        if (!*presult)
            return PYGEN_ERROR;
        return r == QP_SEND_NEXT ? PYGEN_NEXT : PYGEN_RETURN;
    }
}

/* ---- dedicated reverse bridges of static types ---------------------------- */

static QPType *qp_capi_dedicated_qt[QP_CAPI_NDEDICATED];
static int qp_capi_dedicated_count;
static QPCapiPMap qp_capi_dedicated_index;  /* static PyTypeObject* -> index + 1 (process lifetime) */

typedef struct QPCapiDedicated {
    newfunc tp_new;
    initproc tp_init;
    reprfunc tp_repr, tp_str;
    hashfunc tp_hash;
    richcmpfunc tp_richcompare;
    getattrofunc tp_getattro;
    setattrofunc tp_setattro;
    getiterfunc tp_iter;
    iternextfunc tp_iternext;
    ternaryfunc tp_call;
    descrgetfunc tp_descr_get;
    descrsetfunc tp_descr_set;
} QPCapiDedicated;

#define QP_CAPI_DEDICATED_FUNCS(I)                                                                                     \
    static PyObject *qp_capi_rbd##I##_new(PyTypeObject *s, PyObject *a, PyObject *k) { return qp_capi_rb_new_impl(qp_capi_dedicated_qt[I], s, a, k); } \
    static int qp_capi_rbd##I##_init(PyObject *o, PyObject *a, PyObject *k) { return qp_capi_rb_init_impl(qp_capi_dedicated_qt[I], o, a, k); } \
    static PyObject *qp_capi_rbd##I##_repr(PyObject *o) { return qp_capi_rb_repr_impl(qp_capi_dedicated_qt[I], o); } \
    static PyObject *qp_capi_rbd##I##_str(PyObject *o) { return qp_capi_rb_str_impl(qp_capi_dedicated_qt[I], o); } \
    static Py_hash_t qp_capi_rbd##I##_hash(PyObject *o) { return qp_capi_rb_hash_impl(qp_capi_dedicated_qt[I], o); } \
    static PyObject *qp_capi_rbd##I##_richcompare(PyObject *a, PyObject *b, int op) { return qp_capi_rb_richcompare_impl(qp_capi_dedicated_qt[I], a, b, op); } \
    static PyObject *qp_capi_rbd##I##_getattro(PyObject *o, PyObject *n) { return qp_capi_rb_getattro_impl(qp_capi_dedicated_qt[I], o, n); } \
    static int qp_capi_rbd##I##_setattro(PyObject *o, PyObject *n, PyObject *v) { return qp_capi_rb_setattro_impl(qp_capi_dedicated_qt[I], o, n, v); } \
    static PyObject *qp_capi_rbd##I##_iter(PyObject *o) { return qp_capi_rb_iter_impl(qp_capi_dedicated_qt[I], o); } \
    static PyObject *qp_capi_rbd##I##_iternext(PyObject *o) { return qp_capi_rb_iternext_impl(qp_capi_dedicated_qt[I], o); } \
    static PyObject *qp_capi_rbd##I##_call(PyObject *o, PyObject *a, PyObject *k) { return qp_capi_rb_call_impl(qp_capi_dedicated_qt[I], o, a, k); } \
    static PyObject *qp_capi_rbd##I##_descr_get(PyObject *d, PyObject *o, PyObject *t) { return qp_capi_rb_descr_get_impl(qp_capi_dedicated_qt[I], d, o, t); } \
    static int qp_capi_rbd##I##_descr_set(PyObject *d, PyObject *o, PyObject *v) { return qp_capi_rb_descr_set_impl(qp_capi_dedicated_qt[I], d, o, v); }

#define QP_CAPI_DEDICATED_ENTRY(I)                                                \
    { qp_capi_rbd##I##_new, qp_capi_rbd##I##_init, qp_capi_rbd##I##_repr,         \
      qp_capi_rbd##I##_str, qp_capi_rbd##I##_hash, qp_capi_rbd##I##_richcompare,  \
      qp_capi_rbd##I##_getattro, qp_capi_rbd##I##_setattro, qp_capi_rbd##I##_iter, \
      qp_capi_rbd##I##_iternext, qp_capi_rbd##I##_call, qp_capi_rbd##I##_descr_get, \
      qp_capi_rbd##I##_descr_set },

#define QP_CAPI_D10(X, p) X(p##0) X(p##1) X(p##2) X(p##3) X(p##4) X(p##5) X(p##6) X(p##7) X(p##8) X(p##9)
#define QP_CAPI_DEDICATED_INDICES(X)                                                \
    QP_CAPI_D10(X, ) QP_CAPI_D10(X, 1) QP_CAPI_D10(X, 2) QP_CAPI_D10(X, 3)         \
    QP_CAPI_D10(X, 4) QP_CAPI_D10(X, 5) QP_CAPI_D10(X, 6) QP_CAPI_D10(X, 7)         \
    QP_CAPI_D10(X, 8) QP_CAPI_D10(X, 9) QP_CAPI_D10(X, 10) QP_CAPI_D10(X, 11)       \
    QP_CAPI_D10(X, 12) QP_CAPI_D10(X, 13) QP_CAPI_D10(X, 14) QP_CAPI_D10(X, 15)     \
    QP_CAPI_D10(X, 16) QP_CAPI_D10(X, 17) QP_CAPI_D10(X, 18) X(190) X(191)

QP_CAPI_DEDICATED_INDICES(QP_CAPI_DEDICATED_FUNCS)

static const QPCapiDedicated qp_capi_dedicated[QP_CAPI_NDEDICATED] = {
    QP_CAPI_DEDICATED_INDICES(QP_CAPI_DEDICATED_ENTRY)
};

/* ========================================================================= */
/* Static type objects                                                       */
/* ========================================================================= */

/* The module object (CPython 3.15 Include/internal/pycore_moduleobject.h,
   GIL build; capi/compat/pycore_moduleobject.h has the same layout) plus
   B's private fields.  Materialized by the MODULE kind (capi_module.c). */
typedef struct QPCapiModuleObject {
    PyObject_HEAD
    PyObject *md_dict;
    void *md_state;
    PyObject *md_weaklist;
    PyObject *md_name;
    bool md_token_is_def;
    Py_ssize_t md_state_size;
    traverseproc md_state_traverse;
    inquiry md_state_clear;
    freefunc md_state_free;
    void *md_token;
    int (*md_exec)(PyObject *);
    /* private */
    PyObject *qp_state_owner;   /* single-phase init: the module whose state this one shares */
    uint8_t qp_state_freed;     /* m_free / md_state released */
} QPCapiModuleObject;

#define QP_CAPI_GC Py_TPFLAGS_HAVE_GC
#define QP_CAPI_OPAQUE(n) (sizeof(PyObject) + (n) * sizeof(PyObject *))

/* X(symbol, tp_name, basicsize, itemsize, C-only flags[, initializers]).
   Every entry of QP_CAPI_STATIC_TYPES (capi.h) plus the C-API-only and
   never-instantiated types owned by B.  Sizes are CPython's where the
   struct is public; tp_dictoffset / tp_weaklistoffset only where the C
   field is maintained (the others report managed ones, qp_type_layout).
   Cython's limited-API code reads a type's dict at type.__dictoffset__. */
#define QP_CAPI_STATIC_DEFS(X)                                                                \
    X(PyBaseObject_Type, "object", sizeof(PyObject), 0, 0)                                   \
    X(PyType_Type, "type", sizeof(PyHeapTypeObject), sizeof(PyMemberDef),                    \
      QP_CAPI_GC | Py_TPFLAGS_ITEMS_AT_END | Py_TPFLAGS_HAVE_VECTORCALL,                     \
      .tp_vectorcall_offset = offsetof(PyTypeObject, tp_vectorcall),                        \
      .tp_dictoffset = offsetof(PyTypeObject, tp_dict),                                     \
      .tp_weaklistoffset = offsetof(PyTypeObject, tp_weaklist))                             \
    X(PySuper_Type, "super", QP_CAPI_OPAQUE(3), 0, QP_CAPI_GC)                               \
    X(PyProperty_Type, "property", QP_CAPI_OPAQUE(6), 0, QP_CAPI_GC)                         \
    X(PyClassMethod_Type, "classmethod", QP_CAPI_OPAQUE(2), 0, QP_CAPI_GC)                   \
    X(PyStaticMethod_Type, "staticmethod", QP_CAPI_OPAQUE(2), 0, QP_CAPI_GC)                 \
    X(PyCFunction_Type, "builtin_function_or_method", sizeof(PyCFunctionObject), 0, QP_CAPI_GC) \
    X(PyCMethod_Type, "builtin_method", sizeof(PyCMethodObject), 0, QP_CAPI_GC)              \
    X(PyMethodDescr_Type, "method_descriptor", sizeof(PyMethodDescrObject), 0, QP_CAPI_GC)   \
    X(PyClassMethodDescr_Type, "classmethod_descriptor", sizeof(PyMethodDescrObject), 0, QP_CAPI_GC) \
    X(PyGetSetDescr_Type, "getset_descriptor", sizeof(PyGetSetDescrObject), 0, QP_CAPI_GC)   \
    X(PyMemberDescr_Type, "member_descriptor", sizeof(PyMemberDescrObject), 0, QP_CAPI_GC)   \
    X(PyWrapperDescr_Type, "wrapper_descriptor", sizeof(PyWrapperDescrObject), 0, QP_CAPI_GC) \
    X(_PyNone_Type, "NoneType", 0, 0, 0)                                                     \
    X(_PyNotImplemented_Type, "NotImplementedType", 0, 0, 0)                                 \
    X(PyFunction_Type, "function", sizeof(PyFunctionObject), 0, QP_CAPI_GC)                  \
    X(PyMethod_Type, "method", sizeof(PyMethodObject), 0, QP_CAPI_GC)                        \
    X(PyCode_Type, "code", sizeof(PyCodeObject), 0, 0)                                       \
    X(PyCell_Type, "cell", sizeof(PyCellObject), 0, QP_CAPI_GC)                              \
    X(PyFrame_Type, "frame", QP_CAPI_OPAQUE(4), 0, QP_CAPI_GC)                               \
    X(PyLong_Type, "int", offsetof(PyLongObject, long_value.ob_digit), sizeof(digit), 0)    \
    X(PyBool_Type, "bool", offsetof(PyLongObject, long_value.ob_digit), sizeof(digit), 0)   \
    X(PyFloat_Type, "float", sizeof(PyFloatObject), 0, 0)                                    \
    X(PyComplex_Type, "complex", sizeof(PyComplexObject), 0, 0)                              \
    X(PyUnicode_Type, "str", sizeof(PyUnicodeObject), 0, 0)                                  \
    X(PyList_Type, "list", sizeof(PyListObject), 0, QP_CAPI_GC)                              \
    X(PyTuple_Type, "tuple", sizeof(PyTupleObject) - sizeof(PyObject *), sizeof(PyObject *), QP_CAPI_GC) \
    X(PyRange_Type, "range", QP_CAPI_OPAQUE(4), 0, 0)                                        \
    X(PySlice_Type, "slice", sizeof(PySliceObject), 0, QP_CAPI_GC)                           \
    X(PyEllipsis_Type, "ellipsis", 0, 0, 0)                                                  \
    X(PyListIter_Type, "list_iterator", QP_CAPI_OPAQUE(2), 0, QP_CAPI_GC)                    \
    X(PyListRevIter_Type, "list_reverseiterator", QP_CAPI_OPAQUE(2), 0, QP_CAPI_GC)          \
    X(PyTupleIter_Type, "tuple_iterator", QP_CAPI_OPAQUE(2), 0, QP_CAPI_GC)                  \
    X(PyRangeIter_Type, "range_iterator", QP_CAPI_OPAQUE(3), 0, 0)                           \
    X(PyLongRangeIter_Type, "longrange_iterator", QP_CAPI_OPAQUE(3), 0, QP_CAPI_GC)          \
    X(PyUnicodeIter_Type, "str_iterator", QP_CAPI_OPAQUE(2), 0, QP_CAPI_GC)                  \
    X(PyBytesIter_Type, "bytes_iterator", QP_CAPI_OPAQUE(2), 0, QP_CAPI_GC)                  \
    X(PyByteArrayIter_Type, "bytearray_iterator", QP_CAPI_OPAQUE(2), 0, QP_CAPI_GC)          \
    X(PySeqIter_Type, "iterator", QP_CAPI_OPAQUE(2), 0, QP_CAPI_GC)                          \
    X(PyCallIter_Type, "callable_iterator", QP_CAPI_OPAQUE(2), 0, QP_CAPI_GC)                \
    X(PyDict_Type, "dict", sizeof(PyDictObject), 0, QP_CAPI_GC)                              \
    X(PyFrozenDict_Type, "frozendict", sizeof(PyDictObject) + sizeof(Py_hash_t), 0, QP_CAPI_GC) \
    X(PyDictKeys_Type, "dict_keys", QP_CAPI_OPAQUE(1), 0, QP_CAPI_GC)                        \
    X(PyDictValues_Type, "dict_values", QP_CAPI_OPAQUE(1), 0, QP_CAPI_GC)                    \
    X(PyDictItems_Type, "dict_items", QP_CAPI_OPAQUE(1), 0, QP_CAPI_GC)                      \
    X(PyDictIterKey_Type, "dict_keyiterator", QP_CAPI_OPAQUE(4), 0, QP_CAPI_GC)              \
    X(PyDictIterValue_Type, "dict_valueiterator", QP_CAPI_OPAQUE(4), 0, QP_CAPI_GC)          \
    X(PyDictIterItem_Type, "dict_itemiterator", QP_CAPI_OPAQUE(4), 0, QP_CAPI_GC)            \
    X(PyDictRevIterKey_Type, "dict_reversekeyiterator", QP_CAPI_OPAQUE(4), 0, QP_CAPI_GC)    \
    X(PyDictRevIterValue_Type, "dict_reversevalueiterator", QP_CAPI_OPAQUE(4), 0, QP_CAPI_GC) \
    X(PyDictRevIterItem_Type, "dict_reverseitemiterator", QP_CAPI_OPAQUE(4), 0, QP_CAPI_GC)  \
    X(PyDictProxy_Type, "mappingproxy", QP_CAPI_OPAQUE(1), 0, QP_CAPI_GC)                    \
    X(PySet_Type, "set", sizeof(PySetObject), 0, QP_CAPI_GC)                                 \
    X(PyFrozenSet_Type, "frozenset", sizeof(PySetObject), 0, QP_CAPI_GC)                     \
    X(PySetIter_Type, "set_iterator", QP_CAPI_OPAQUE(4), 0, QP_CAPI_GC)                      \
    X(PyBytes_Type, "bytes", offsetof(PyBytesObject, _qp_sval) + 1, 1, 0)                   \
    X(PyByteArray_Type, "bytearray", sizeof(PyByteArrayObject), 0, 0)                        \
    X(PyMemoryView_Type, "memoryview", offsetof(PyMemoryViewObject, ob_array), sizeof(Py_ssize_t), QP_CAPI_GC) \
    X(PyTraceBack_Type, "traceback", QP_CAPI_OPAQUE(4), 0, QP_CAPI_GC)                       \
    X(PyGen_Type, "generator", QP_CAPI_OPAQUE(8), 0, QP_CAPI_GC)                             \
    X(PyCoro_Type, "coroutine", QP_CAPI_OPAQUE(8), 0, QP_CAPI_GC)                            \
    X(PyAsyncGen_Type, "async_generator", QP_CAPI_OPAQUE(8), 0, QP_CAPI_GC)                  \
    X(PyEnum_Type, "enumerate", QP_CAPI_OPAQUE(4), 0, QP_CAPI_GC)                            \
    X(PyReversed_Type, "reversed", QP_CAPI_OPAQUE(2), 0, QP_CAPI_GC)                         \
    X(PyFilter_Type, "filter", QP_CAPI_OPAQUE(2), 0, QP_CAPI_GC)                             \
    X(PyMap_Type, "map", QP_CAPI_OPAQUE(3), 0, QP_CAPI_GC)                                   \
    X(PyZip_Type, "zip", QP_CAPI_OPAQUE(3), 0, QP_CAPI_GC)                                   \
    X(Py_GenericAliasType, "types.GenericAlias", QP_CAPI_OPAQUE(6), 0, QP_CAPI_GC)           \
    X(PySentinel_Type, "sentinel", QP_CAPI_OPAQUE(2), 0, 0)                                  \
    X(_PyAsyncGenASend_Type, "async_generator_asend", QP_CAPI_OPAQUE(4), 0, QP_CAPI_GC)      \
    X(PyModule_Type, "module", sizeof(QPCapiModuleObject), 0, QP_CAPI_GC,                    \
      .tp_dictoffset = offsetof(QPCapiModuleObject, md_dict),                               \
      .tp_weaklistoffset = offsetof(QPCapiModuleObject, md_weaklist))                       \
    /* PyODictObject: PyDictObject + 8 pointer-sized fields */                               \
    X(PyODict_Type, "collections.OrderedDict", sizeof(PyDictObject) + 8 * sizeof(void *), 0, \
      QP_CAPI_GC)                                                                            \
    X(PyODictIter_Type, "odict_iterator", QP_CAPI_OPAQUE(6), 0, QP_CAPI_GC)                  \
    X(PyODictKeys_Type, "odict_keys", QP_CAPI_OPAQUE(1), 0, QP_CAPI_GC)                      \
    X(PyODictItems_Type, "odict_items", QP_CAPI_OPAQUE(1), 0, QP_CAPI_GC)                    \
    X(PyODictValues_Type, "odict_values", QP_CAPI_OPAQUE(1), 0, QP_CAPI_GC)                  \
    /* never instantiated (no frame locals proxy) */                                         \
    X(PyFrameLocalsProxy_Type, "FrameLocalsProxy", QP_CAPI_OPAQUE(1), 0, QP_CAPI_GC)

#define QP_CAPI_STATIC_DEF(sym, name, size, isize, cflags, ...)           \
    PyTypeObject sym = {                                                  \
        .ob_base = PyVarObject_HEAD_INIT(&PyType_Type, 0)                 \
        .tp_name = name,                                                  \
        .tp_basicsize = (Py_ssize_t)(size),                               \
        .tp_itemsize = (Py_ssize_t)(isize),                               \
        .tp_flags = Py_TPFLAGS_DEFAULT | _Py_TPFLAGS_STATIC_BUILTIN | (cflags), \
        __VA_ARGS__                                                       \
    };
QP_CAPI_STATIC_DEFS(QP_CAPI_STATIC_DEF)
#undef QP_CAPI_STATIC_DEF

/* bases that CPython sets statically (the others come from the QP base) */
static void qp_capi_static_bases_init(void)
{
    PyBool_Type.tp_base = &PyLong_Type;
    PyCMethod_Type.tp_base = &PyCFunction_Type;
}

/* ========================================================================= */
/* Slot map: QP slot <-> C slot, bridges                                      */
/* ========================================================================= */

typedef struct QPCapiSlotMapEntry {
    uint16_t qoff;              /* offsetof(QPTypeSlots, ...) */
    int16_t suite;              /* -1: PyTypeObject field, else offsetof(PyTypeObject, tp_as_*) */
    uint16_t coff;              /* offset of the C field in PyTypeObject / its suite */
    int16_t doff;               /* offset in QPCapiDedicated, -1 none */
    void *rb;                   /* generic reverse bridge */
    void *fwd;                  /* forward bridge */
} QPCapiSlotMapEntry;

/* forward bridges (defined below) */
static JSValue qp_capi_fwd_getattro(JSContext *ctx, JSValueConst obj, JSAtom name);
static int qp_capi_fwd_setattro(JSContext *ctx, JSValueConst obj, JSAtom name, JSValueConst v);
static JSValue qp_capi_fwd_repr(JSContext *ctx, JSValueConst a);
static JSValue qp_capi_fwd_str(JSContext *ctx, JSValueConst a);
static Py_hash_t qp_capi_fwd_hash(JSContext *ctx, JSValueConst a);
static JSValue qp_capi_fwd_call(JSContext *ctx, JSValueConst callable, JSValueConst *args,
                                size_t nargsf, JSValueConst kwnames);
static JSValue qp_capi_fwd_richcompare(JSContext *ctx, JSValueConst a, JSValueConst b, int op);
static JSValue qp_capi_fwd_iter(JSContext *ctx, JSValueConst a);
static JSValue qp_capi_fwd_iternext(JSContext *ctx, JSValueConst a);
static JSValue qp_capi_fwd_descr_get(JSContext *ctx, JSValueConst d, JSValueConst obj, JSValueConst type);
static int qp_capi_fwd_descr_set(JSContext *ctx, JSValueConst d, JSValueConst obj, JSValueConst v);
static int qp_capi_fwd_init(JSContext *ctx, JSValueConst self, JSValueConst *args, size_t nargsf,
                            JSValueConst kwnames);
static JSValue qp_capi_fwd_new(JSContext *ctx, QPType *subtype, JSValueConst *args, size_t nargsf,
                               JSValueConst kwnames);
static int qp_capi_fwd_nb_bool(JSContext *ctx, JSValueConst a);
static Py_ssize_t qp_capi_fwd_sq_length(JSContext *ctx, JSValueConst a);
static Py_ssize_t qp_capi_fwd_mp_length(JSContext *ctx, JSValueConst a);
static JSValue qp_capi_fwd_sq_repeat(JSContext *ctx, JSValueConst a, Py_ssize_t i);
static JSValue qp_capi_fwd_sq_item(JSContext *ctx, JSValueConst a, Py_ssize_t i);
static JSValue qp_capi_fwd_sq_inplace_repeat(JSContext *ctx, JSValueConst a, Py_ssize_t i);
static int qp_capi_fwd_sq_ass_item(JSContext *ctx, JSValueConst a, Py_ssize_t i, JSValueConst v);
static int qp_capi_fwd_sq_contains(JSContext *ctx, JSValueConst a, JSValueConst b);
static int qp_capi_fwd_mp_ass_subscript(JSContext *ctx, JSValueConst a, JSValueConst k, JSValueConst v);
static int qp_capi_fwd_am_send(JSContext *ctx, JSValueConst it, JSValueConst arg, JSValue *presult);

#define QP_CAPI_FWD_DECL_UNARY(S, N) static JSValue qp_capi_fwd_##N(JSContext *ctx, JSValueConst a);
#define QP_CAPI_FWD_DECL_BINARY(S, N) static JSValue qp_capi_fwd_##N(JSContext *ctx, JSValueConst a, JSValueConst b);
#define QP_CAPI_FWD_DECL_TERNARY(S, N) static JSValue qp_capi_fwd_##N(JSContext *ctx, JSValueConst a, JSValueConst b, JSValueConst c);

/* suite slots with a uniform shape: X_UNARY / X_BINARY (both operands) /
   X_BINARY1 (dispatch on the first operand only) / X_TERNARY */
#define QP_CAPI_SUITE_SLOTS(XU, XB, XB1, XT)                                  \
    XB(nb, nb_add) XB(nb, nb_subtract) XB(nb, nb_multiply) XB(nb, nb_remainder) \
    XB(nb, nb_divmod) XT(nb, nb_power) XU(nb, nb_negative) XU(nb, nb_positive) \
    XU(nb, nb_absolute) XU(nb, nb_invert) XB(nb, nb_lshift) XB(nb, nb_rshift)  \
    XB(nb, nb_and) XB(nb, nb_xor) XB(nb, nb_or) XU(nb, nb_int) XU(nb, nb_float) \
    XB1(nb, nb_inplace_add) XB1(nb, nb_inplace_subtract)                        \
    XB1(nb, nb_inplace_multiply) XB1(nb, nb_inplace_remainder)                  \
    XT(nb, nb_inplace_power) XB1(nb, nb_inplace_lshift)                         \
    XB1(nb, nb_inplace_rshift) XB1(nb, nb_inplace_and) XB1(nb, nb_inplace_xor)  \
    XB1(nb, nb_inplace_or) XB(nb, nb_floor_divide) XB(nb, nb_true_divide)       \
    XB1(nb, nb_inplace_floor_divide) XB1(nb, nb_inplace_true_divide)            \
    XU(nb, nb_index) XB(nb, nb_matrix_multiply)                                 \
    XB1(nb, nb_inplace_matrix_multiply) XB1(sq, sq_concat)                      \
    XB1(sq, sq_inplace_concat) XB1(mp, mp_subscript) XU(am, am_await)           \
    XU(am, am_aiter) XU(am, am_anext)

QP_CAPI_SUITE_SLOTS(QP_CAPI_FWD_DECL_UNARY, QP_CAPI_FWD_DECL_BINARY, QP_CAPI_FWD_DECL_BINARY,
                    QP_CAPI_FWD_DECL_TERNARY)

#define QP_CAPI_SUITE_OF_nb tp_as_number
#define QP_CAPI_SUITE_OF_sq tp_as_sequence
#define QP_CAPI_SUITE_OF_mp tp_as_mapping
#define QP_CAPI_SUITE_OF_am tp_as_async
#define QP_CAPI_CSUITE_nb PyNumberMethods
#define QP_CAPI_CSUITE_sq PySequenceMethods
#define QP_CAPI_CSUITE_mp PyMappingMethods
#define QP_CAPI_CSUITE_am PyAsyncMethods
#define QP_CAPI_CAT(a, b) a##b
#define QP_CAPI_SUITE_OFF(S) offsetof(PyTypeObject, QP_CAPI_CAT(QP_CAPI_SUITE_OF_, S))
#define QP_CAPI_CSUITE(S) QP_CAPI_CAT(QP_CAPI_CSUITE_, S)

#define QP_CAPI_TP_ENTRY(QF, CF, DF, RB, FWD)                                       \
    { QP_CAPI_QOFF(QF), -1, offsetof(PyTypeObject, CF),                             \
      DF, (void *)(RB), (void *)(FWD) },
#define QP_CAPI_SUITE_ENTRY(S, N, RB, FWD)                                          \
    { QP_CAPI_QOFF(S.N), (int16_t)QP_CAPI_SUITE_OFF(S),                             \
      offsetof(QP_CAPI_CSUITE(S), N), -1, (void *)(RB), (void *)(FWD) },
#define QP_CAPI_SUITE_ENTRY_STD(S, N) QP_CAPI_SUITE_ENTRY(S, N, qp_capi_rbg_##N, qp_capi_fwd_##N)
#define QP_CAPI_DOFF(F) (int16_t)offsetof(QPCapiDedicated, F)

static const QPCapiSlotMapEntry qp_capi_slot_map[] = {
    QP_CAPI_TP_ENTRY(tp_getattro, tp_getattro, QP_CAPI_DOFF(tp_getattro), qp_capi_rbg_getattro, qp_capi_fwd_getattro)
    QP_CAPI_TP_ENTRY(tp_setattro, tp_setattro, QP_CAPI_DOFF(tp_setattro), qp_capi_rbg_setattro, qp_capi_fwd_setattro)
    QP_CAPI_TP_ENTRY(tp_repr, tp_repr, QP_CAPI_DOFF(tp_repr), qp_capi_rbg_repr, qp_capi_fwd_repr)
    QP_CAPI_TP_ENTRY(tp_str, tp_str, QP_CAPI_DOFF(tp_str), qp_capi_rbg_str, qp_capi_fwd_str)
    QP_CAPI_TP_ENTRY(tp_hash, tp_hash, QP_CAPI_DOFF(tp_hash), qp_capi_rbg_hash, qp_capi_fwd_hash)
    QP_CAPI_TP_ENTRY(tp_call, tp_call, QP_CAPI_DOFF(tp_call), qp_capi_rbg_call, qp_capi_fwd_call)
    QP_CAPI_TP_ENTRY(tp_richcompare, tp_richcompare, QP_CAPI_DOFF(tp_richcompare), qp_capi_rbg_richcompare, qp_capi_fwd_richcompare)
    QP_CAPI_TP_ENTRY(tp_iter, tp_iter, QP_CAPI_DOFF(tp_iter), qp_capi_rbg_iter, qp_capi_fwd_iter)
    QP_CAPI_TP_ENTRY(tp_iternext, tp_iternext, QP_CAPI_DOFF(tp_iternext), qp_capi_rbg_iternext, qp_capi_fwd_iternext)
    QP_CAPI_TP_ENTRY(tp_descr_get, tp_descr_get, QP_CAPI_DOFF(tp_descr_get), qp_capi_rbg_descr_get, qp_capi_fwd_descr_get)
    QP_CAPI_TP_ENTRY(tp_descr_set, tp_descr_set, QP_CAPI_DOFF(tp_descr_set), qp_capi_rbg_descr_set, qp_capi_fwd_descr_set)
    QP_CAPI_TP_ENTRY(tp_init, tp_init, QP_CAPI_DOFF(tp_init), qp_capi_rbg_init, qp_capi_fwd_init)
    QP_CAPI_TP_ENTRY(tp_new, tp_new, QP_CAPI_DOFF(tp_new), qp_capi_rbg_new, qp_capi_fwd_new)
#define QP_CAPI_SM_STD(S, N) QP_CAPI_SUITE_ENTRY_STD(S, N)
    QP_CAPI_SUITE_SLOTS(QP_CAPI_SM_STD, QP_CAPI_SM_STD, QP_CAPI_SM_STD, QP_CAPI_SM_STD)
#undef QP_CAPI_SM_STD
    QP_CAPI_SUITE_ENTRY(nb, nb_bool, qp_capi_rbg_nb_bool, qp_capi_fwd_nb_bool)
    QP_CAPI_SUITE_ENTRY(sq, sq_length, qp_capi_rbg_sq_length, qp_capi_fwd_sq_length)
    QP_CAPI_SUITE_ENTRY(sq, sq_repeat, qp_capi_rbg_sq_repeat, qp_capi_fwd_sq_repeat)
    QP_CAPI_SUITE_ENTRY(sq, sq_item, qp_capi_rbg_sq_item, qp_capi_fwd_sq_item)
    QP_CAPI_SUITE_ENTRY(sq, sq_ass_item, qp_capi_rbg_sq_ass_item, qp_capi_fwd_sq_ass_item)
    QP_CAPI_SUITE_ENTRY(sq, sq_contains, qp_capi_rbg_sq_contains, qp_capi_fwd_sq_contains)
    QP_CAPI_SUITE_ENTRY(sq, sq_inplace_repeat, qp_capi_rbg_sq_inplace_repeat, qp_capi_fwd_sq_inplace_repeat)
    QP_CAPI_SUITE_ENTRY(mp, mp_length, qp_capi_rbg_mp_length, qp_capi_fwd_mp_length)
    QP_CAPI_SUITE_ENTRY(mp, mp_ass_subscript, qp_capi_rbg_mp_ass_subscript, qp_capi_fwd_mp_ass_subscript)
    QP_CAPI_SUITE_ENTRY(am, am_send, qp_capi_rbg_am_send, qp_capi_fwd_am_send)
};

/* C functions with a direct QP equivalent (no bridge in either direction) */
static const struct { size_t qoff; void *cfn; void *qfn; } qp_capi_known_slots[] = {
    { QP_CAPI_QOFF(tp_getattro), (void *)PyObject_GenericGetAttr, (void *)qp_generic_getattr },
    { QP_CAPI_QOFF(tp_setattro), (void *)PyObject_GenericSetAttr, (void *)qp_generic_setattr },
    { QP_CAPI_QOFF(tp_hash), (void *)PyObject_HashNotImplemented, (void *)qp_hash_not_implemented },
};

static void *qp_capi_known_q2c(size_t qoff, void *qfn)
{
    size_t i;
    for (i = 0; i < countof(qp_capi_known_slots); i++)
        if (qp_capi_known_slots[i].qoff == qoff && qp_capi_known_slots[i].qfn == qfn)
            return qp_capi_known_slots[i].cfn;
    return NULL;
}

static void *qp_capi_known_c2q(size_t qoff, void *cfn)
{
    size_t i;
    for (i = 0; i < countof(qp_capi_known_slots); i++)
        if (qp_capi_known_slots[i].qoff == qoff && qp_capi_known_slots[i].cfn == cfn)
            return qp_capi_known_slots[i].qfn;
    return NULL;
}

/* Address of the C field of entry e in tp; for suites, the suite is
   attached to the record's storage when missing and `create` is set
   (else NULL). */
static void **qp_capi_cslot_ptr(PyTypeObject *tp, const QPCapiSlotMapEntry *e, int create)
{
    uint8_t *suite;
    if (e->suite < 0)
        return (void **)((uint8_t *)tp + e->coff);
    suite = *(uint8_t **)((uint8_t *)tp + e->suite);
    if (!suite) {
        QPCapiTypeInfo *ti;
        if (!create || !(ti = qp_capi_tinfo(tp)))
            return NULL;
        if (e->suite == (int16_t)offsetof(PyTypeObject, tp_as_number))
            suite = (uint8_t *)(tp->tp_as_number = &ti->as_number);
        else if (e->suite == (int16_t)offsetof(PyTypeObject, tp_as_sequence))
            suite = (uint8_t *)(tp->tp_as_sequence = &ti->as_sequence);
        else if (e->suite == (int16_t)offsetof(PyTypeObject, tp_as_mapping))
            suite = (uint8_t *)(tp->tp_as_mapping = &ti->as_mapping);
        else
            suite = (uint8_t *)(tp->tp_as_async = &ti->as_async);
    }
    return (void **)(suite + e->coff);
}

static void *qp_capi_cslot_get(PyTypeObject *tp, const QPCapiSlotMapEntry *e)
{
    void **p = qp_capi_cslot_ptr(tp, e, 0);
    return p ? *p : NULL;
}

static int qp_capi_is_c_defined(PyTypeObject *tp)
{
    QPCapiTypeInfo *ti = qp_capi_tinfo(tp);
    return ti && (ti->origin == QP_CAPI_TO_CSTATIC || ti->origin == QP_CAPI_TO_CHEAP);
}

/* The C-defined type whose C slot implements forward bridge `fwd` (QP slot
   at qoff) for objects of type t: the nearest type of t's MRO that is
   C-defined and has this forward bridge (Python subclasses inherit it). */
static PyTypeObject *qp_capi_fwd_owner(QPType *t, size_t qoff, void *fwd)
{
    PyTypeObject *tp;
    Py_ssize_t i, n;
    JSValue *items;

    if (QP_CAPI_QSLOT(t, qoff) == fwd && (tp = qp_capi_type_map_get(t)) && qp_capi_is_c_defined(tp))
        return tp;
    if (JS_VALUE_GET_TAG(t->mro) == JS_TAG_OBJECT) {
        n = QP_TUPLE_SIZE(t->mro);
        items = QP_TUPLE_ITEMS(t->mro);
        for (i = 1; i < n; i++) {
            QPType *u = qp_type_from_value(items[i]);
            if (u && QP_CAPI_QSLOT(u, qoff) == fwd && (tp = qp_capi_type_map_get(u)) &&
                qp_capi_is_c_defined(tp))
                return tp;
        }
        return NULL;
    }
    for (t = t->base; t; t = t->base) {
        if (QP_CAPI_QSLOT(t, qoff) == fwd && (tp = qp_capi_type_map_get(t)) && qp_capi_is_c_defined(tp))
            return tp;
    }
    return NULL;
}

/* Nearest builtin static type in the MRO of t (t excluded) whose QP slot
   is qf: a class inheriting a builtin's slot inherits that type's C slot,
   so C code comparing slots (Argument Clinic's
   `type->tp_init == base_tp->tp_init`) sees CPython's inheritance. */
static PyTypeObject *qp_capi_builtin_slot_owner(QPType *t, size_t qoff, void *qf)
{
    PyTypeObject *tp;
    QPCapiTypeInfo *ti;
    Py_ssize_t i, n;
    JSValue *items;

    if (JS_VALUE_GET_TAG(t->mro) != JS_TAG_OBJECT)
        return NULL;
    n = QP_TUPLE_SIZE(t->mro);
    items = QP_TUPLE_ITEMS(t->mro);
    for (i = 1; i < n; i++) {
        QPType *u = qp_type_from_value(items[i]);
        if (!u || QP_CAPI_QSLOT(u, qoff) != qf)
            continue;
        if ((tp = qp_capi_type_map_get(u)) && (ti = qp_capi_tinfo(tp)) &&
            ti->origin == QP_CAPI_TO_BUILTIN)
            return tp;
    }
    return NULL;
}

static const QPCapiSlotMapEntry *qp_capi_slot_entry(size_t qoff)
{
    size_t i;
    for (i = 0; i < countof(qp_capi_slot_map); i++)
        if (qp_capi_slot_map[i].qoff == qoff)
            return &qp_capi_slot_map[i];
    return NULL;
}

/* The type of the slot wrapper being called for QP slot qoff
   (qp_wrapper_raw_call) when that slot is a forward bridge, else NULL.
   The wrapper type stays recorded while a wrapped slot that is not a
   forward bridge runs (object.__ne__ calls type(self).__eq__, which can be
   a forward bridge of another type): only the bridge it wraps uses it. */
static QPType *qp_capi_wrapper_pending(JSContext *ctx, size_t qoff)
{
    QPState *s = QP_STATE(ctx);
    QPType *w = s->wrapper_type;
    if (!w || s->wrapper_slot != (int)qoff || !qp_capi_is_fwd(QP_CAPI_QSLOT(w, qoff)))
        return NULL;
    return w;
}

/* C function of slot `qoff` for objects of QP type t reached from forward
   bridge fwd; NULL + SystemError if none. */
/* The type whose C slot a forward bridge of QP slot qoff runs for an object
   of type t: t, or the type of the slot wrapper being called for this slot
   (Base.__init__(self) runs Base's C tp_init; descrobject.c
   qp_wrapper_raw_call), which the call consumes. */
static QPType *qp_capi_fwd_type(JSContext *ctx, QPType *t, size_t qoff)
{
    QPType *w = qp_capi_wrapper_pending(ctx, qoff);
    if (w) {
        if (qp_type_is_subtype(t, w))
            t = w;
        QP_STATE(ctx)->wrapper_type = NULL;
    }
    return t;
}

static void *qp_capi_fwd_cfunc(JSContext *ctx, QPType *t, size_t qoff, void *fwd)
{
    const QPCapiSlotMapEntry *e = qp_capi_slot_entry(qoff);
    PyTypeObject *tp = qp_capi_fwd_owner(qp_capi_fwd_type(ctx, t, qoff), qoff, fwd);
    void *f = (tp && e) ? qp_capi_cslot_get(tp, e) : NULL;
    if (!f)
        qp_raise_system_error(ctx, "C slot bridge: no C implementation for '%N'", t);
    return f;
}

/* ========================================================================= */
/* Forward bridges: QP slot call -> C slot of the C-defined type              */
/* ========================================================================= */

static JSValue qp_capi_fwd_unary(JSContext *ctx, JSValueConst a, size_t qoff, void *fwd)
{
    unaryfunc f = (unaryfunc)qp_capi_fwd_cfunc(ctx, qp_type_of(ctx, a), qoff, fwd);
    PyObject *x, *r;
    if (!f)
        return JS_EXCEPTION;
    x = qp_capi_new_ref(ctx, a);
    if (!x)
        return JS_EXCEPTION;
    r = f(x);
    Py_DECREF(x);
    return qp_capi_result_to_js(ctx, r, "C slot");
}

/* C functions of a binary slot for both operands (both = 0: the first
   operand only: in-place forms, sq_concat, mp_subscript). */
static int qp_capi_fwd_pair(JSContext *ctx, JSValueConst a, JSValueConst b, size_t qoff, void *fwd,
                            int both, void **pfa, void **pfb)
{
    const QPCapiSlotMapEntry *e = qp_capi_slot_entry(qoff);
    QPType *ta = qp_type_of(ctx, a), *tb, *w;
    QPState *s = QP_STATE(ctx);
    PyTypeObject *o;
    *pfa = *pfb = NULL;
    /* called by the slot wrapper of a C-defined type (Base.__getitem__(self, i),
       Base.__radd__(self, x)): its C function only, whatever type(self) has */
    if ((w = qp_capi_wrapper_pending(ctx, qoff))) {
        s->wrapper_type = NULL;
        if (qp_type_is_subtype(ta, w) && (o = qp_capi_fwd_owner(w, qoff, fwd)))
            *pfa = qp_capi_cslot_get(o, e);
        else if (both && qp_type_is_subtype(qp_type_of(ctx, b), w) &&
                 (o = qp_capi_fwd_owner(w, qoff, fwd)))
            *pfb = qp_capi_cslot_get(o, e);
        if (*pfa || *pfb)
            return 0;
    }
    if (QP_CAPI_QSLOT(ta, qoff) == fwd && (o = qp_capi_fwd_owner(ta, qoff, fwd)))
        *pfa = qp_capi_cslot_get(o, e);
    if (both) {
        tb = qp_type_of(ctx, b);
        if (tb != ta && QP_CAPI_QSLOT(tb, qoff) == fwd && (o = qp_capi_fwd_owner(tb, qoff, fwd)))
            *pfb = qp_capi_cslot_get(o, e);
        if (*pfb == *pfa)
            *pfb = NULL;
    }
    if (!*pfa && !*pfb) {
        qp_raise_system_error(ctx, "C slot bridge: no C implementation for '%N'", ta);
        return -1;
    }
    return 0;
}

static JSValue qp_capi_fwd_binary(JSContext *ctx, JSValueConst a, JSValueConst b, size_t qoff,
                                  void *fwd, int both)
{
    void *fa, *fb;
    PyObject *x, *y, *r;
    if (qp_capi_fwd_pair(ctx, a, b, qoff, fwd, both, &fa, &fb) < 0)
        return JS_EXCEPTION;
    x = qp_capi_new_ref(ctx, a);
    if (!x)
        return JS_EXCEPTION;
    y = qp_capi_new_ref(ctx, b);
    if (!y) {
        Py_DECREF(x);
        return JS_EXCEPTION;
    }
    if (fa) {
        r = ((binaryfunc)fa)(x, y);
        if (r == Py_NotImplemented && fb) {
            Py_DECREF(r);
            r = ((binaryfunc)fb)(x, y);
        }
    } else {
        r = ((binaryfunc)fb)(x, y);
    }
    Py_DECREF(x);
    Py_DECREF(y);
    return qp_capi_result_to_js(ctx, r, "C slot");
}

static JSValue qp_capi_fwd_ternary(JSContext *ctx, JSValueConst a, JSValueConst b, JSValueConst c,
                                   size_t qoff, void *fwd, int both)
{
    void *fa, *fb;
    PyObject *x, *y, *z, *r;
    if (both && QP_CAPI_QSLOT(qp_type_of(ctx, a), qoff) != fwd &&
        QP_CAPI_QSLOT(qp_type_of(ctx, b), qoff) != fwd &&
        !qp_capi_wrapper_pending(ctx, qoff)) {
        /* ternary_op's last try: the slot of the type of z
           (pow(10, 2, Decimal(7))) */
        fa = qp_capi_fwd_cfunc(ctx, qp_type_of(ctx, c), qoff, fwd);
        fb = NULL;
        if (!fa)
            return JS_EXCEPTION;
    } else if (qp_capi_fwd_pair(ctx, a, b, qoff, fwd, both, &fa, &fb) < 0) {
        return JS_EXCEPTION;
    }
    x = qp_capi_new_ref(ctx, a);
    y = x ? qp_capi_new_ref(ctx, b) : NULL;
    z = y ? qp_capi_new_ref(ctx, c) : NULL;
    if (!z) {
        Py_XDECREF(x);
        Py_XDECREF(y);
        return JS_EXCEPTION;
    }
    if (fa) {
        r = ((ternaryfunc)fa)(x, y, z);
        if (r == Py_NotImplemented && fb) {
            Py_DECREF(r);
            r = ((ternaryfunc)fb)(x, y, z);
        }
    } else {
        r = ((ternaryfunc)fb)(x, y, z);
    }
    Py_DECREF(x);
    Py_DECREF(y);
    Py_DECREF(z);
    return qp_capi_result_to_js(ctx, r, "C slot");
}

#define QP_CAPI_FWD_DEF_UNARY(S, N)                                                   \
    static JSValue qp_capi_fwd_##N(JSContext *ctx, JSValueConst a)                    \
    { return qp_capi_fwd_unary(ctx, a, QP_CAPI_QOFF(S.N), (void *)qp_capi_fwd_##N); }
#define QP_CAPI_FWD_DEF_BINARY(S, N)                                                  \
    static JSValue qp_capi_fwd_##N(JSContext *ctx, JSValueConst a, JSValueConst b)    \
    { return qp_capi_fwd_binary(ctx, a, b, QP_CAPI_QOFF(S.N), (void *)qp_capi_fwd_##N, 1); }
#define QP_CAPI_FWD_DEF_BINARY1(S, N)                                                 \
    static JSValue qp_capi_fwd_##N(JSContext *ctx, JSValueConst a, JSValueConst b)    \
    { return qp_capi_fwd_binary(ctx, a, b, QP_CAPI_QOFF(S.N), (void *)qp_capi_fwd_##N, 0); }
#define QP_CAPI_FWD_DEF_TERNARY(S, N)                                                 \
    static JSValue qp_capi_fwd_##N(JSContext *ctx, JSValueConst a, JSValueConst b,    \
                                   JSValueConst c)                                     \
    { return qp_capi_fwd_ternary(ctx, a, b, c, QP_CAPI_QOFF(S.N), (void *)qp_capi_fwd_##N, \
                                 QP_CAPI_QOFF(S.N) == QP_CAPI_QOFF(nb.nb_power)); }

QP_CAPI_SUITE_SLOTS(QP_CAPI_FWD_DEF_UNARY, QP_CAPI_FWD_DEF_BINARY, QP_CAPI_FWD_DEF_BINARY1,
                    QP_CAPI_FWD_DEF_TERNARY)

static JSValue qp_capi_fwd_repr(JSContext *ctx, JSValueConst a)
{
    return qp_capi_fwd_unary(ctx, a, QP_CAPI_QOFF(tp_repr), (void *)qp_capi_fwd_repr);
}

static JSValue qp_capi_fwd_str(JSContext *ctx, JSValueConst a)
{
    return qp_capi_fwd_unary(ctx, a, QP_CAPI_QOFF(tp_str), (void *)qp_capi_fwd_str);
}

static JSValue qp_capi_fwd_iter(JSContext *ctx, JSValueConst a)
{
    return qp_capi_fwd_unary(ctx, a, QP_CAPI_QOFF(tp_iter), (void *)qp_capi_fwd_iter);
}

static JSValue qp_capi_fwd_iternext(JSContext *ctx, JSValueConst a)
{
    iternextfunc f = (iternextfunc)qp_capi_fwd_cfunc(ctx, qp_type_of(ctx, a), QP_CAPI_QOFF(tp_iternext),
                                                    (void *)qp_capi_fwd_iternext);
    PyObject *x, *r;
    JSValue v;
    if (!f)
        return JS_EXCEPTION;
    x = qp_capi_new_ref(ctx, a);
    if (!x)
        return JS_EXCEPTION;
    r = f(x);
    Py_DECREF(x);
    if (!r) {
        if (!JS_HasException(ctx))
            return QP_NULL;
        if (PyErr_ExceptionMatches(PyExc_StopIteration)) {
            PyErr_Clear();
            return QP_NULL;
        }
        return JS_EXCEPTION;
    }
    v = qp_capi_to_js(ctx, r);
    Py_DECREF(r);
    return v;
}

static Py_hash_t qp_capi_fwd_hash(JSContext *ctx, JSValueConst a)
{
    hashfunc f = (hashfunc)qp_capi_fwd_cfunc(ctx, qp_type_of(ctx, a), QP_CAPI_QOFF(tp_hash),
                                            (void *)qp_capi_fwd_hash);
    PyObject *x;
    Py_hash_t h;
    if (!f)
        return -1;
    x = qp_capi_new_ref(ctx, a);
    if (!x)
        return -1;
    h = f(x);
    Py_DECREF(x);
    if (h == -1 && !JS_HasException(ctx))
        h = -2;
    return h;
}

static JSValue qp_capi_fwd_richcompare(JSContext *ctx, JSValueConst a, JSValueConst b, int op)
{
    richcmpfunc f = (richcmpfunc)qp_capi_fwd_cfunc(ctx, qp_type_of(ctx, a), QP_CAPI_QOFF(tp_richcompare),
                                                  (void *)qp_capi_fwd_richcompare);
    PyObject *x, *y, *r;
    if (!f)
        return JS_EXCEPTION;
    x = qp_capi_new_ref(ctx, a);
    if (!x)
        return JS_EXCEPTION;
    y = qp_capi_new_ref(ctx, b);
    if (!y) {
        Py_DECREF(x);
        return JS_EXCEPTION;
    }
    r = f(x, y, op);
    Py_DECREF(x);
    Py_DECREF(y);
    return qp_capi_result_to_js(ctx, r, "tp_richcompare");
}

static JSValue qp_capi_fwd_getattro(JSContext *ctx, JSValueConst obj, JSAtom name)
{
    QPType *t = qp_capi_fwd_type(ctx, qp_type_of(ctx, obj), QP_CAPI_QOFF(tp_getattro));
    PyTypeObject *tp = qp_capi_fwd_owner(t, QP_CAPI_QOFF(tp_getattro), (void *)qp_capi_fwd_getattro);
    PyObject *x, *n, *r;
    if (!tp || (!tp->tp_getattro && !tp->tp_getattr))
        return qp_raise_system_error(ctx, "C slot bridge: no C implementation for '%N'", t);
    x = qp_capi_new_ref(ctx, obj);
    if (!x)
        return JS_EXCEPTION;
    n = qp_capi_steal(ctx, qp_str_from_atom(ctx, name));
    if (!n) {
        Py_DECREF(x);
        return JS_EXCEPTION;
    }
    if (tp->tp_getattro) {
        r = tp->tp_getattro(x, n);
    } else {
        const char *s = PyUnicode_AsUTF8(n);
        r = s ? tp->tp_getattr(x, (char *)s) : NULL;
    }
    Py_DECREF(n);
    Py_DECREF(x);
    return qp_capi_result_to_js(ctx, r, "tp_getattro");
}

static int qp_capi_fwd_setattro(JSContext *ctx, JSValueConst obj, JSAtom name, JSValueConst v)
{
    QPType *t = qp_capi_fwd_type(ctx, qp_type_of(ctx, obj), QP_CAPI_QOFF(tp_setattro));
    PyTypeObject *tp = qp_capi_fwd_owner(t, QP_CAPI_QOFF(tp_setattro), (void *)qp_capi_fwd_setattro);
    PyObject *x, *n, *val = NULL;
    int r;
    if (!tp || (!tp->tp_setattro && !tp->tp_setattr)) {
        qp_raise_system_error(ctx, "C slot bridge: no C implementation for '%N'", t);
        return -1;
    }
    x = qp_capi_new_ref(ctx, obj);
    if (!x)
        return -1;
    n = qp_capi_steal(ctx, qp_str_from_atom(ctx, name));
    if (!n) {
        Py_DECREF(x);
        return -1;
    }
    if (!QP_IS_NULL(v)) {
        val = qp_capi_new_ref(ctx, v);
        if (!val) {
            Py_DECREF(n);
            Py_DECREF(x);
            return -1;
        }
    }
    if (tp->tp_setattro) {
        r = tp->tp_setattro(x, n, val);
    } else {
        const char *s = PyUnicode_AsUTF8(n);
        r = s ? tp->tp_setattr(x, (char *)s, val) : -1;
    }
    Py_XDECREF(val);
    Py_DECREF(n);
    Py_DECREF(x);
    return r < 0 ? -1 : 0;
}

/* QPTypeSlots.tp_vectorcall of a C type with tp_vectorcall (Py_tp_vectorcall):
   calling the type object itself (CPython's type tp_vectorcall_offset). */
static JSValue qp_capi_fwd_type_vectorcall(JSContext *ctx, JSValueConst callable,
                                           JSValueConst *args, size_t nargsf,
                                           JSValueConst kwnames)
{
    int nargs = (int)qp_nargs(nargsf);
    int nkw = QP_IS_NULL(kwnames) ? 0 : (int)QP_TUPLE_SIZE(kwnames);
    PyObject *x, *kwn = NULL, *r = NULL;
    QPCapiArgs a;
    JSValue res;

    x = qp_capi_new_ref(ctx, callable);
    if (!x)
        return JS_EXCEPTION;
    if (!PyType_Check(x) || !((PyTypeObject *)x)->tp_vectorcall) {
        Py_DECREF(x);
        return qp_raise_system_error(ctx, "C slot bridge: no tp_vectorcall for '%T'", callable);
    }
    if (qp_capi_args_init(ctx, &a, args, nargs + nkw) == 0 &&
        (!nkw || (kwn = qp_capi_new_ref(ctx, kwnames)) != NULL))
        r = ((PyTypeObject *)x)->tp_vectorcall(x, a.v + 1,
                                               (size_t)nargs | PY_VECTORCALL_ARGUMENTS_OFFSET, kwn);
    Py_XDECREF(kwn);
    qp_capi_args_free(ctx, &a);
    res = qp_capi_result_to_js(ctx, r, ((PyTypeObject *)x)->tp_name);
    Py_DECREF(x);
    return res;
}

static JSValue qp_capi_fwd_call(JSContext *ctx, JSValueConst callable, JSValueConst *args,
                                size_t nargsf, JSValueConst kwnames)
{
    QPType *t = qp_capi_fwd_type(ctx, qp_type_of(ctx, callable), QP_CAPI_QOFF(tp_call));
    PyTypeObject *tp = qp_capi_fwd_owner(t, QP_CAPI_QOFF(tp_call), (void *)qp_capi_fwd_call);
    int nargs = (int)qp_nargs(nargsf);
    int nkw = QP_IS_NULL(kwnames) ? 0 : (int)QP_TUPLE_SIZE(kwnames);
    PyObject *x, *r = NULL, *tup, *kw, *kwn;
    vectorcallfunc vf = NULL;

    if (!tp)
        return qp_raise_system_error(ctx, "C slot bridge: no C implementation for '%N'", t);
    x = qp_capi_new_ref(ctx, callable);
    if (!x)
        return JS_EXCEPTION;
    if ((tp->tp_flags & Py_TPFLAGS_HAVE_VECTORCALL) && tp->tp_vectorcall_offset > 0)
        vf = *(vectorcallfunc *)((uint8_t *)x + tp->tp_vectorcall_offset);
    if (vf) {
        QPCapiArgs a;
        kwn = NULL;
        if (qp_capi_args_init(ctx, &a, args, nargs + nkw) == 0 &&
            (!nkw || (kwn = qp_capi_new_ref(ctx, kwnames)) != NULL))
            r = vf(x, a.v + 1, (size_t)nargs | PY_VECTORCALL_ARGUMENTS_OFFSET, kwn);
        else
            r = NULL;
        Py_XDECREF(kwn);
        qp_capi_args_free(ctx, &a);
    } else if (tp->tp_call) {
        if (qp_capi_vc_to_c(ctx, args, nargs, kwnames, &tup, &kw) == 0) {
            r = tp->tp_call(x, tup, kw);
            Py_DECREF(tup);
            Py_XDECREF(kw);
        }
    } else {
        Py_DECREF(x);
        return qp_raise_type_error(ctx, "'%T' object is not callable", callable);
    }
    Py_DECREF(x);
    return qp_capi_result_to_js(ctx, r, tp->tp_name);
}

static JSValue qp_capi_fwd_descr_get(JSContext *ctx, JSValueConst d, JSValueConst obj, JSValueConst type)
{
    descrgetfunc f = (descrgetfunc)qp_capi_fwd_cfunc(ctx, qp_type_of(ctx, d), QP_CAPI_QOFF(tp_descr_get),
                                                    (void *)qp_capi_fwd_descr_get);
    PyObject *x, *o = NULL, *t = NULL, *r;
    if (!f)
        return JS_EXCEPTION;
    x = qp_capi_new_ref(ctx, d);
    if (!x)
        return JS_EXCEPTION;
    if (!QP_IS_NULL(obj) && !(o = qp_capi_new_ref(ctx, obj)))
        goto fail;
    if (!QP_IS_NULL(type) && !(t = qp_capi_new_ref(ctx, type)))
        goto fail;
    r = f(x, o, t);
    Py_XDECREF(t);
    Py_XDECREF(o);
    Py_DECREF(x);
    return qp_capi_result_to_js(ctx, r, "tp_descr_get");
fail:
    Py_XDECREF(o);
    Py_DECREF(x);
    return JS_EXCEPTION;
}

static int qp_capi_fwd_descr_set(JSContext *ctx, JSValueConst d, JSValueConst obj, JSValueConst v)
{
    descrsetfunc f = (descrsetfunc)qp_capi_fwd_cfunc(ctx, qp_type_of(ctx, d), QP_CAPI_QOFF(tp_descr_set),
                                                    (void *)qp_capi_fwd_descr_set);
    PyObject *x, *o, *val = NULL;
    int r;
    if (!f)
        return -1;
    x = qp_capi_new_ref(ctx, d);
    if (!x)
        return -1;
    o = qp_capi_new_ref(ctx, obj);
    if (!o) {
        Py_DECREF(x);
        return -1;
    }
    if (!QP_IS_NULL(v) && !(val = qp_capi_new_ref(ctx, v))) {
        Py_DECREF(o);
        Py_DECREF(x);
        return -1;
    }
    r = f(x, o, val);
    Py_XDECREF(val);
    Py_DECREF(o);
    Py_DECREF(x);
    return r < 0 ? -1 : 0;
}

static int qp_capi_fwd_init(JSContext *ctx, JSValueConst self, JSValueConst *args, size_t nargsf,
                            JSValueConst kwnames)
{
    initproc f = (initproc)qp_capi_fwd_cfunc(ctx, qp_type_of(ctx, self), QP_CAPI_QOFF(tp_init),
                                            (void *)qp_capi_fwd_init);
    PyObject *x, *tup, *kw;
    int r;
    if (!f)
        return -1;
    x = qp_capi_new_ref(ctx, self);
    if (!x)
        return -1;
    if (qp_capi_vc_to_c(ctx, args, (int)qp_nargs(nargsf), kwnames, &tup, &kw) < 0) {
        Py_DECREF(x);
        return -1;
    }
    r = f(x, tup, kw);
    Py_DECREF(tup);
    Py_XDECREF(kw);
    Py_DECREF(x);
    if (r < 0) {
        if (!JS_HasException(ctx))
            PyErr_SetString(PyExc_SystemError, "__init__ returned -1 without setting an exception");
        return -1;
    }
    return 0;
}

static JSValue qp_capi_fwd_new(JSContext *ctx, QPType *subtype, JSValueConst *args, size_t nargsf,
                               JSValueConst kwnames)
{
    newfunc f = (newfunc)qp_capi_fwd_cfunc(ctx, subtype, QP_CAPI_QOFF(tp_new), (void *)qp_capi_fwd_new);
    PyTypeObject *sub;
    PyObject *tup, *kw, *r;
    if (!f)
        return JS_EXCEPTION;
    sub = qp_capi_type_from_qp(ctx, subtype);
    if (!sub)
        return JS_EXCEPTION;
    if (qp_capi_vc_to_c(ctx, args, (int)qp_nargs(nargsf), kwnames, &tup, &kw) < 0)
        return JS_EXCEPTION;
    Py_INCREF(sub);
    r = f(sub, tup, kw);
    Py_DECREF(sub);
    Py_DECREF(tup);
    Py_XDECREF(kw);
    return qp_capi_result_to_js(ctx, r, "tp_new");
}

static int qp_capi_fwd_nb_bool(JSContext *ctx, JSValueConst a)
{
    inquiry f = (inquiry)qp_capi_fwd_cfunc(ctx, qp_type_of(ctx, a), QP_CAPI_QOFF(nb.nb_bool),
                                          (void *)qp_capi_fwd_nb_bool);
    PyObject *x;
    int r;
    if (!f)
        return -1;
    x = qp_capi_new_ref(ctx, a);
    if (!x)
        return -1;
    r = f(x);
    Py_DECREF(x);
    return r < 0 ? -1 : r != 0;
}

static Py_ssize_t qp_capi_fwd_len(JSContext *ctx, JSValueConst a, size_t qoff, void *fwd)
{
    lenfunc f = (lenfunc)qp_capi_fwd_cfunc(ctx, qp_type_of(ctx, a), qoff, fwd);
    PyObject *x;
    Py_ssize_t r;
    if (!f)
        return -1;
    x = qp_capi_new_ref(ctx, a);
    if (!x)
        return -1;
    r = f(x);
    Py_DECREF(x);
    return r;
}

static Py_ssize_t qp_capi_fwd_sq_length(JSContext *ctx, JSValueConst a)
{
    return qp_capi_fwd_len(ctx, a, QP_CAPI_QOFF(sq.sq_length), (void *)qp_capi_fwd_sq_length);
}

static Py_ssize_t qp_capi_fwd_mp_length(JSContext *ctx, JSValueConst a)
{
    return qp_capi_fwd_len(ctx, a, QP_CAPI_QOFF(mp.mp_length), (void *)qp_capi_fwd_mp_length);
}

static JSValue qp_capi_fwd_ssizearg(JSContext *ctx, JSValueConst a, Py_ssize_t i, size_t qoff, void *fwd)
{
    ssizeargfunc f = (ssizeargfunc)qp_capi_fwd_cfunc(ctx, qp_type_of(ctx, a), qoff, fwd);
    PyObject *x, *r;
    if (!f)
        return JS_EXCEPTION;
    x = qp_capi_new_ref(ctx, a);
    if (!x)
        return JS_EXCEPTION;
    r = f(x, i);
    Py_DECREF(x);
    return qp_capi_result_to_js(ctx, r, "C slot");
}

static JSValue qp_capi_fwd_sq_repeat(JSContext *ctx, JSValueConst a, Py_ssize_t i)
{
    return qp_capi_fwd_ssizearg(ctx, a, i, QP_CAPI_QOFF(sq.sq_repeat), (void *)qp_capi_fwd_sq_repeat);
}

static JSValue qp_capi_fwd_sq_item(JSContext *ctx, JSValueConst a, Py_ssize_t i)
{
    return qp_capi_fwd_ssizearg(ctx, a, i, QP_CAPI_QOFF(sq.sq_item), (void *)qp_capi_fwd_sq_item);
}

static JSValue qp_capi_fwd_sq_inplace_repeat(JSContext *ctx, JSValueConst a, Py_ssize_t i)
{
    return qp_capi_fwd_ssizearg(ctx, a, i, QP_CAPI_QOFF(sq.sq_inplace_repeat),
                                (void *)qp_capi_fwd_sq_inplace_repeat);
}

static int qp_capi_fwd_sq_ass_item(JSContext *ctx, JSValueConst a, Py_ssize_t i, JSValueConst v)
{
    ssizeobjargproc f = (ssizeobjargproc)qp_capi_fwd_cfunc(ctx, qp_type_of(ctx, a),
                                                          QP_CAPI_QOFF(sq.sq_ass_item),
                                                          (void *)qp_capi_fwd_sq_ass_item);
    PyObject *x, *val = NULL;
    int r;
    if (!f)
        return -1;
    x = qp_capi_new_ref(ctx, a);
    if (!x)
        return -1;
    if (!QP_IS_NULL(v) && !(val = qp_capi_new_ref(ctx, v))) {
        Py_DECREF(x);
        return -1;
    }
    r = f(x, i, val);
    Py_XDECREF(val);
    Py_DECREF(x);
    return r < 0 ? -1 : 0;
}

static int qp_capi_fwd_sq_contains(JSContext *ctx, JSValueConst a, JSValueConst b)
{
    objobjproc f = (objobjproc)qp_capi_fwd_cfunc(ctx, qp_type_of(ctx, a), QP_CAPI_QOFF(sq.sq_contains),
                                                (void *)qp_capi_fwd_sq_contains);
    PyObject *x, *y;
    int r;
    if (!f)
        return -1;
    x = qp_capi_new_ref(ctx, a);
    if (!x)
        return -1;
    y = qp_capi_new_ref(ctx, b);
    if (!y) {
        Py_DECREF(x);
        return -1;
    }
    r = f(x, y);
    Py_DECREF(y);
    Py_DECREF(x);
    return r < 0 ? -1 : r != 0;
}

static int qp_capi_fwd_mp_ass_subscript(JSContext *ctx, JSValueConst a, JSValueConst k, JSValueConst v)
{
    objobjargproc f = (objobjargproc)qp_capi_fwd_cfunc(ctx, qp_type_of(ctx, a),
                                                      QP_CAPI_QOFF(mp.mp_ass_subscript),
                                                      (void *)qp_capi_fwd_mp_ass_subscript);
    PyObject *x, *y, *val = NULL;
    int r;
    if (!f)
        return -1;
    x = qp_capi_new_ref(ctx, a);
    if (!x)
        return -1;
    y = qp_capi_new_ref(ctx, k);
    if (!y) {
        Py_DECREF(x);
        return -1;
    }
    if (!QP_IS_NULL(v) && !(val = qp_capi_new_ref(ctx, v))) {
        Py_DECREF(y);
        Py_DECREF(x);
        return -1;
    }
    r = f(x, y, val);
    Py_XDECREF(val);
    Py_DECREF(y);
    Py_DECREF(x);
    return r < 0 ? -1 : 0;
}

static int qp_capi_fwd_am_send(JSContext *ctx, JSValueConst it, JSValueConst arg, JSValue *presult)
{
    sendfunc f = (sendfunc)qp_capi_fwd_cfunc(ctx, qp_type_of(ctx, it), QP_CAPI_QOFF(am.am_send),
                                            (void *)qp_capi_fwd_am_send);
    PyObject *x, *y, *res = NULL;
    PySendResult r;
    *presult = QP_NULL;
    if (!f)
        return QP_SEND_ERROR;
    x = qp_capi_new_ref(ctx, it);
    if (!x)
        return QP_SEND_ERROR;
    y = qp_capi_new_ref(ctx, arg);
    if (!y) {
        Py_DECREF(x);
        return QP_SEND_ERROR;
    }
    r = f(x, y, &res);
    Py_DECREF(y);
    Py_DECREF(x);
    if (r == PYGEN_ERROR) {
        Py_XDECREF(res);
        if (!JS_HasException(ctx))
            PyErr_SetString(PyExc_SystemError, "am_send failed without setting an exception");
        return QP_SEND_ERROR;
    }
    *presult = res ? qp_capi_to_js(ctx, res) : JS_UNDEFINED;
    Py_XDECREF(res);
    if (JS_IsException(*presult)) {
        *presult = QP_NULL;
        return QP_SEND_ERROR;
    }
    return r == PYGEN_NEXT ? QP_SEND_NEXT : QP_SEND_RETURN;
}

/* ========================================================================= */
/* Type records, flags, builtin slots                                         */
/* ========================================================================= */

/* tp_flags bits that exist only on the C side (kept on refresh) */
#define QP_CAPI_FLAGS_C_ONLY                                                   \
    ((unsigned long)(_Py_TPFLAGS_STATIC_BUILTIN | Py_TPFLAGS_INLINE_VALUES |    \
                     Py_TPFLAGS_MANAGED_WEAKREF | Py_TPFLAGS_HAVE_VECTORCALL |  \
                     Py_TPFLAGS_HAVE_GC | Py_TPFLAGS_HAVE_VERSION_TAG |         \
                     Py_TPFLAGS_ITEMS_AT_END | (3UL << 15)))
#define QP_CAPI_FLAGS_SHARED (0xFFFFFFFFUL & ~QP_CAPI_FLAGS_C_ONLY)

/* The compiled-in state of a static type (and of the slot suites it points
   to) before its first binding, restored by qp_capi_types_teardown so that
   the C API of a later runtime binds and readies it again from scratch.
   Process lifetime. */
typedef struct QPCapiTypeSave {
    PyTypeObject tp;
    PyAsyncMethods as_async;
    PyNumberMethods as_number;
    PySequenceMethods as_sequence;
    PyMappingMethods as_mapping;
    PyBufferProcs as_buffer;
} QPCapiTypeSave;

static QPCapiPMap qp_capi_type_pristine;    /* static PyTypeObject* -> QPCapiTypeSave* */

static int qp_capi_type_save(PyTypeObject *tp)
{
    QPCapiTypeSave *sv;

    if (qp_capi_pmap_get(&qp_capi_type_pristine, tp, NULL))
        return 0;
    sv = qp_sys_calloc(1, sizeof(*sv));
    if (!sv)
        return -1;
    sv->tp = *tp;
    if (tp->tp_as_async)
        sv->as_async = *tp->tp_as_async;
    if (tp->tp_as_number)
        sv->as_number = *tp->tp_as_number;
    if (tp->tp_as_sequence)
        sv->as_sequence = *tp->tp_as_sequence;
    if (tp->tp_as_mapping)
        sv->as_mapping = *tp->tp_as_mapping;
    if (tp->tp_as_buffer)
        sv->as_buffer = *tp->tp_as_buffer;
    if (qp_capi_pmap_set(&qp_capi_type_pristine, tp, NULL, sv) < 0) {
        qp_sys_free(sv);
        return -1;
    }
    return 0;
}

static void qp_capi_type_restore(PyTypeObject *tp)
{
    QPCapiTypeSave *sv = qp_capi_pmap_get(&qp_capi_type_pristine, tp, NULL);

    if (!sv)
        return;
    *tp = sv->tp;
    if (tp->tp_as_async)
        *tp->tp_as_async = sv->as_async;
    if (tp->tp_as_number)
        *tp->tp_as_number = sv->as_number;
    if (tp->tp_as_sequence)
        *tp->tp_as_sequence = sv->as_sequence;
    if (tp->tp_as_mapping)
        *tp->tp_as_mapping = sv->as_mapping;
    if (tp->tp_as_buffer)
        *tp->tp_as_buffer = sv->as_buffer;
}

static QPCapiTypeInfo *qp_capi_tinfo_new(PyTypeObject *tp, int origin)
{
    QPCapiTypeInfo *ti;

    if ((origin == QP_CAPI_TO_BUILTIN || origin == QP_CAPI_TO_CSTATIC) &&
        qp_capi_type_save(tp) < 0) {
        PyErr_NoMemory();
        return NULL;
    }
    ti = qp_sys_calloc(1, sizeof(*ti));
    if (!ti) {
        PyErr_NoMemory();
        return NULL;
    }
    ti->magic = QP_CAPI_TI_MAGIC;
    ti->origin = origin;
    ti->dedicated = -1;
    ti->tp = tp;
    ti->j = QP_NULL;
    tp->tp_subclasses = ti;
    return ti;
}

static void qp_capi_tinfo_free(PyTypeObject *tp)
{
    QPCapiTypeInfo *ti = qp_capi_tinfo(tp);
    if (!ti)
        return;
    if (ti->qt && qp_capi_type_map_get(ti->qt) == tp)
        qp_capi_pmap_del(&qp_capi_type_pmap, ti->qt, NULL);
    qp_sys_free(ti->methods);
    qp_sys_free(ti->getsets);
    qp_sys_free(ti->name_buf);
    qp_sys_free(ti->doc_buf);
    if (tp->tp_doc == ti->doc_buf)
        tp->tp_doc = NULL;
    if (tp->tp_name == ti->name_buf)
        tp->tp_name = "?";
    if ((QPType *)tp->tp_cache == ti->qt)
        tp->tp_cache = NULL;
    ti->magic = 0;
    tp->tp_subclasses = NULL;
    qp_sys_free(ti);
}

/* tp_flags of a bound type from its QPType */
static unsigned long qp_capi_flags_from_qp(QPType *t, unsigned long c_bits)
{
    unsigned long f = ((unsigned long)t->flags & QP_CAPI_FLAGS_SHARED) | (c_bits & QP_CAPI_FLAGS_C_ONLY);
    /* the runtime's method-cache state; CPython 3.15 never sets the bit */
    f &= ~Py_TPFLAGS_VALID_VERSION_TAG;
    /* static builtin types keep their __dict__ at tp_dictoffset (a managed
       dict requires a heap type: type_ready_managed_dict) */
    if (c_bits & _Py_TPFLAGS_STATIC_BUILTIN)
        f &= ~Py_TPFLAGS_MANAGED_DICT;
    if (t->flags & QP_TPFLAGS_READY)
        f |= Py_TPFLAGS_READY;
    return f;
}

/* QP flags of a C-defined type */
static uint64_t qp_capi_flags_to_qp(PyTypeObject *tp)
{
    uint64_t f = tp->tp_flags & QP_CAPI_FLAGS_SHARED;
    f &= ~(uint64_t)(Py_TPFLAGS_READY | Py_TPFLAGS_READYING | Py_TPFLAGS_HAVE_FINALIZE |
                     Py_TPFLAGS_VALID_VERSION_TAG);
    if (tp->tp_finalize || tp->tp_del)
        f |= QP_TPFLAGS_HAS_DEL;
    if (tp->tp_weaklistoffset || (tp->tp_flags & Py_TPFLAGS_MANAGED_WEAKREF))
        f |= QP_TPFLAGS_WEAKREF;
    if (tp->tp_dictoffset || (tp->tp_flags & Py_TPFLAGS_MANAGED_DICT))
        f |= QP_TPFLAGS_MANAGED_DICT;
    /* tp_itemsize != 0: no arbitrary __slots__ and no implicit __weakref__
       in Python subclasses (type_new_descriptors, type_new_slots_impl) */
    if (tp->tp_itemsize)
        f |= QP_TPFLAGS_PRIV_VARSIZE;
    return f;
}

/* ---- builtin tp_dealloc / tp_traverse / tp_clear (what C subclasses chain to) */

static void qp_capi_builtin_dealloc(PyObject *op)
{
    PyTypeObject *tp = Py_TYPE(op);
    if (QP_CAPI_HAS_HEAD(op)) {
        QPCapiHead *h = QP_CAPI_HEAD(op);
        if (h->kind < QP_CAPI_K__COUNT && qp_capi.kind_ops[h->kind].clear)
            qp_capi.kind_ops[h->kind].clear(qp_capi.rt, op);
    }
    if (tp->tp_flags & Py_TPFLAGS_HAVE_GC)
        PyObject_GC_UnTrack(op);
    tp->tp_free(op);
}

static int qp_capi_builtin_traverse(PyObject *op, visitproc visit, void *arg)
{
    if (QP_CAPI_HAS_HEAD(op)) {
        QPCapiHead *h = QP_CAPI_HEAD(op);
        if (h->kind < QP_CAPI_K__COUNT && qp_capi.kind_ops[h->kind].traverse)
            return qp_capi.kind_ops[h->kind].traverse(op, visit, arg);
    }
    return 0;
}

/* dict_tp_clear / list_clear / set_clear: empty the container */
static int qp_capi_builtin_clear(PyObject *op)
{
    QPCapiHead *h;
    if (!QP_CAPI_HAS_HEAD(op))
        return 0;
    h = QP_CAPI_HEAD(op);
    switch (h->kind) {
    case QP_CAPI_K_DICT:
        PyDict_Clear(op);
        break;
    case QP_CAPI_K_LIST:
        if (PyList_SetSlice(op, 0, PY_SSIZE_T_MAX, NULL) < 0)
            PyErr_Clear();
        break;
    case QP_CAPI_K_SET:
        if (PySet_Clear(op) < 0)
            PyErr_Clear();
        break;
    default:
        break;
    }
    return 0;
}

/* Python subclasses and heap types without tp_dealloc (subtype_dealloc) */
static void qp_capi_subtype_dealloc(PyObject *self)
{
    PyTypeObject *type = Py_TYPE(self), *base = type;
    int type_needs_decref;

    while (base && base->tp_dealloc == qp_capi_subtype_dealloc)
        base = base->tp_base;
    if (type->tp_finalize || type->tp_del) {
        if (PyObject_CallFinalizerFromDealloc(self) < 0)
            return;     /* resurrected */
    }
    if (type->tp_flags & Py_TPFLAGS_HAVE_GC)
        PyObject_GC_UnTrack(self);
    type_needs_decref = (type->tp_flags & Py_TPFLAGS_HEAPTYPE) &&
        !(base && (base->tp_flags & Py_TPFLAGS_HEAPTYPE));
    if (base && base->tp_dealloc) {
        if ((base->tp_flags & Py_TPFLAGS_HAVE_GC) && (type->tp_flags & Py_TPFLAGS_HAVE_GC))
            PyObject_GC_Track(self);    /* the base dealloc untracks */
        base->tp_dealloc(self);
    } else {
        type->tp_free(self);
    }
    if (type_needs_decref)
        Py_DECREF(type);
}

static int qp_capi_subtype_traverse(PyObject *self, visitproc visit, void *arg)
{
    PyTypeObject *type = Py_TYPE(self), *base = type;
    while (base && base->tp_traverse == qp_capi_subtype_traverse)
        base = base->tp_base;
    /* the instance's reference to its heap type (CPython subtype_traverse):
       a heap-type base's traverse visits Py_TYPE(self) itself, and
       qp_capi_subtype_dealloc only releases the type in the other case */
    if ((type->tp_flags & Py_TPFLAGS_HEAPTYPE) &&
        !(base && (base->tp_flags & Py_TPFLAGS_HEAPTYPE)))
        Py_VISIT(type);
    return base && base->tp_traverse ? base->tp_traverse(self, visit, arg) : 0;
}

static int qp_capi_subtype_clear(PyObject *self)
{
    PyTypeObject *base = Py_TYPE(self);
    while (base && base->tp_clear == qp_capi_subtype_clear)
        base = base->tp_base;
    return base && base->tp_clear ? base->tp_clear(self) : 0;
}

/* ---- binding a static PyTypeObject to a runtime type --------------------- */

/* Phase 1: record, tp_cache, type_map, dedicated bridge index. */
static QPCapiTypeInfo *qp_capi_static_register(JSContext *ctx, PyTypeObject *tp, QPType *t, int origin)
{
    QPCapiTypeInfo *ti = qp_capi_tinfo(tp);
    if (ti)
        return ti;
    ti = qp_capi_tinfo_new(tp, origin);
    if (!ti)
        return NULL;
    ti->qt = t;
    ti->j = JS_DupValue(ctx, QP_TYPE_VALUE(t));
    ti->c_flags = tp->tp_flags & QP_CAPI_FLAGS_C_ONLY;
    tp->tp_cache = (PyObject *)t;
    if (!qp_capi_type_map_get(t) &&
        qp_capi_pmap_set(&qp_capi_type_pmap, t, NULL, tp) < 0) {
        PyErr_NoMemory();
        return NULL;
    }
    if (origin == QP_CAPI_TO_BUILTIN) {
        /* the index of a static type never changes (its C slots keep the
           dedicated bridges across interpreter teardown) */
        intptr_t idx = (intptr_t)qp_capi_pmap_get(&qp_capi_dedicated_index, tp, NULL);
        if (idx) {
            ti->dedicated = (int16_t)(idx - 1);
        } else if (qp_capi_dedicated_count < QP_CAPI_NDEDICATED &&
                   qp_capi_pmap_set(&qp_capi_dedicated_index, tp, NULL,
                                    (void *)(intptr_t)(qp_capi_dedicated_count + 1)) == 0) {
            ti->dedicated = (int16_t)qp_capi_dedicated_count++;
        }
        if (ti->dedicated >= 0)
            qp_capi_dedicated_qt[ti->dedicated] = t;
    }
    return ti;
}

/* tp_dict / tp_bases / tp_mro / tp_base of a bound type (owned refs). */
static int qp_capi_type_sync_tuples(JSContext *ctx, PyTypeObject *tp, QPType *t)
{
    PyObject *o;
    if (t->base && !tp->tp_base) {
        PyTypeObject *b = qp_capi_type_from_qp(ctx, t->base);
        if (!b)
            return -1;
        tp->tp_base = (PyTypeObject *)Py_NewRef((PyObject *)b);
    }
#define QP_CAPI_SYNC(F, V)                                                        \
    if (JS_VALUE_GET_TAG(V) == JS_TAG_OBJECT &&                                   \
        (!tp->F || JS_VALUE_GET_PTR(qp_capi_value(ctx, tp->F)) != JS_VALUE_GET_PTR(V))) { \
        o = qp_capi_new_ref(ctx, V);                                              \
        if (!o)                                                                   \
            return -1;                                                            \
        Py_XSETREF(tp->F, o);                                                     \
    }
    QP_CAPI_SYNC(tp_dict, t->dict)
    QP_CAPI_SYNC(tp_bases, t->bases)
    QP_CAPI_SYNC(tp_mro, t->mro)
#undef QP_CAPI_SYNC
    return 0;
}

/* Reverse-bridge slots of a builtin static type (only NULL C slots). */
static void qp_capi_fill_builtin_slots(QPCapiTypeInfo *ti)
{
    PyTypeObject *tp = ti->tp;
    QPType *t = ti->qt;
    size_t i;

    for (i = 0; i < countof(qp_capi_slot_map); i++) {
        const QPCapiSlotMapEntry *e = &qp_capi_slot_map[i];
        void *qf = QP_CAPI_QSLOT(t, e->qoff), *cf, **p;
        if (!qf)
            continue;
        p = qp_capi_cslot_ptr(tp, e, 1);
        if (!p || *p)
            continue;
        cf = qp_capi_known_q2c(e->qoff, qf);
        if (!cf)
            cf = (e->doff >= 0 && ti->dedicated >= 0)
                ? *(void **)((uint8_t *)&qp_capi_dedicated[ti->dedicated] + e->doff) : e->rb;
        *p = cf;
    }
    if (t->slots.bf.bf_getbuffer && !tp->tp_as_buffer) {
        /* bf_releasebuffer as in CPython (bytes: none; numpy's frombuffer
           wraps exporters that have one in a memoryview); PyBuffer_Release
           frees the reverse bridge's bookkeeping either way */
        ti->as_buffer.bf_getbuffer = qp_capi_bf_getbuffer_rev;
        ti->as_buffer.bf_releasebuffer = t->slots.bf.bf_releasebuffer ? qp_capi_bf_releasebuffer_rev : NULL;
        tp->tp_as_buffer = &ti->as_buffer;
    }
    if (!tp->tp_alloc)
        tp->tp_alloc = PyType_GenericAlloc;
    if (!tp->tp_free)
        tp->tp_free = (tp->tp_flags & Py_TPFLAGS_HAVE_GC) ? PyObject_GC_Del : PyObject_Free;
    if (!tp->tp_dealloc)
        tp->tp_dealloc = qp_capi_builtin_dealloc;
    if ((tp->tp_flags & Py_TPFLAGS_HAVE_GC) && !tp->tp_traverse)
        tp->tp_traverse = qp_capi_builtin_traverse;
    if ((tp->tp_flags & Py_TPFLAGS_HAVE_GC) && !tp->tp_clear)
        tp->tp_clear = qp_capi_builtin_clear;
}

static int qp_capi_static_complete(JSContext *ctx, QPCapiTypeInfo *ti)
{
    PyTypeObject *tp = ti->tp;
    QPType *t = ti->qt;

    if (tp->tp_flags & Py_TPFLAGS_READY)
        return 0;
    tp->tp_flags = qp_capi_flags_from_qp(t, ti->c_flags | tp->tp_flags) | Py_TPFLAGS_READY;
    qp_capi_fill_builtin_slots(ti);
    if (qp_capi_type_sync_tuples(ctx, tp, t) < 0) {
        tp->tp_flags &= ~Py_TPFLAGS_READY;
        return -1;
    }
    tp->tp_version_tag = t->version_tag;
    return 0;
}

static int qp_capi_bind_static_type(JSContext *ctx, PyTypeObject *tp, QPType *t)
{
    QPCapiTypeInfo *ti;
    if (!t) {
        qp_raise_system_error(ctx, "cannot bind static type '%s': runtime type missing", tp->tp_name);
        return -1;
    }
    ti = qp_capi_static_register(ctx, tp, t, QP_CAPI_TO_BUILTIN);
    if (!ti)
        return -1;
    return qp_capi_static_complete(ctx, ti);
}

/* ========================================================================= */
/* Type mirrors (kind TYPE) of runtime / Python types                         */
/* ========================================================================= */

/* The PyTypeObject with C storage that defines the instance layout of t:
   nearest type of the MRO bound statically or C-defined. */
static PyTypeObject *qp_capi_layout_base(QPType *t)
{
    Py_ssize_t i, n;
    JSValue *items;
    PyTypeObject *tp;

    if ((tp = qp_capi_type_map_get(t)))
        return tp;
    if (JS_VALUE_GET_TAG(t->mro) == JS_TAG_OBJECT) {
        n = QP_TUPLE_SIZE(t->mro);
        items = QP_TUPLE_ITEMS(t->mro);
        for (i = 1; i < n; i++) {
            QPType *u = qp_type_from_value(items[i]);
            if (u && (tp = qp_capi_type_map_get(u)))
                return tp;
        }
    } else {
        for (t = t->base; t; t = t->base)
            if ((tp = qp_capi_type_map_get(t)))
                return tp;
    }
    return &PyBaseObject_Type;
}

/* C slot for QP slot qf of mirror type t (NULL when not implemented). */
static void *qp_capi_mirror_cslot(QPType *t, const QPCapiSlotMapEntry *e)
{
    void *qf = QP_CAPI_QSLOT(t, e->qoff), *cf;
    PyTypeObject *owner;
    if (!qf)
        return NULL;
    if (qp_capi_is_fwd(qf)) {
        owner = qp_capi_fwd_owner(t, e->qoff, qf);
        return owner ? qp_capi_cslot_get(owner, e) : NULL;
    }
    cf = qp_capi_known_q2c(e->qoff, qf);
    if (cf)
        return cf;
    owner = qp_capi_builtin_slot_owner(t, e->qoff, qf);
    if (owner && (cf = qp_capi_cslot_get(owner, e)))
        return cf;
    return e->rb;
}

static void qp_capi_mirror_fill_slots(PyTypeObject *tp, QPType *t)
{
    PyHeapTypeObject *ht = (PyHeapTypeObject *)tp;
    PyTypeObject *lb = qp_capi_layout_base(t);
    size_t i;
    void *bf = t->slots.bf.bf_getbuffer;

    tp->tp_as_async = &ht->as_async;
    tp->tp_as_number = &ht->as_number;
    tp->tp_as_sequence = &ht->as_sequence;
    tp->tp_as_mapping = &ht->as_mapping;
    tp->tp_as_buffer = &ht->as_buffer;
    for (i = 0; i < countof(qp_capi_slot_map); i++) {
        const QPCapiSlotMapEntry *e = &qp_capi_slot_map[i];
        *qp_capi_cslot_ptr(tp, e, 0) = qp_capi_mirror_cslot(t, e);
    }
    if (bf == (void *)qp_capi_bf_getbuffer_fwd) {
        PyTypeObject *owner = qp_capi_fwd_owner(t, QP_CAPI_QOFF(bf.bf_getbuffer), bf);
        if (owner && owner->tp_as_buffer)
            ht->as_buffer = *owner->tp_as_buffer;
    } else if (bf) {
        ht->as_buffer.bf_getbuffer = qp_capi_bf_getbuffer_rev;
        ht->as_buffer.bf_releasebuffer = t->slots.bf.bf_releasebuffer ? qp_capi_bf_releasebuffer_rev : NULL;
    } else {
        ht->as_buffer.bf_getbuffer = NULL;
        ht->as_buffer.bf_releasebuffer = NULL;
    }
    tp->tp_alloc = PyType_GenericAlloc;
    tp->tp_dealloc = qp_capi_subtype_dealloc;
    tp->tp_traverse = qp_capi_subtype_traverse;
    tp->tp_clear = qp_capi_subtype_clear;
    tp->tp_free = lb->tp_free ? lb->tp_free : PyObject_GC_Del;
    tp->tp_basicsize = lb->tp_basicsize;
    tp->tp_itemsize = lb->tp_itemsize;
    /* the instance dict stays in the C base's slot (type_new keeps the
       base's tp_dictoffset) */
    if (lb->tp_dictoffset > 0)
        tp->tp_dictoffset = lb->tp_dictoffset;
    tp->tp_finalize = lb->tp_finalize;
    tp->tp_del = lb->tp_del;
    if (QP_CAPI_QSLOT(t, QP_CAPI_QOFF(tp_call)) == (void *)qp_capi_fwd_call &&
        (lb->tp_flags & Py_TPFLAGS_HAVE_VECTORCALL)) {
        tp->tp_vectorcall_offset = lb->tp_vectorcall_offset;
    } else {
        tp->tp_vectorcall_offset = 0;
    }
}

static unsigned long qp_capi_mirror_c_bits(QPType *t)
{
    PyTypeObject *lb = qp_capi_layout_base(t);
    unsigned long f = lb->tp_flags & (Py_TPFLAGS_ITEMS_AT_END | Py_TPFLAGS_HAVE_GC);
    if (t->flags & QP_TPFLAGS_HEAPTYPE) {
        f |= Py_TPFLAGS_HAVE_GC;
        /* type_new_descriptors: a __weakref__ added by a class statement
           is managed */
        if ((t->flags & QP_TPFLAGS_WEAKREF) && lb->tp_weaklistoffset == 0)
            f |= Py_TPFLAGS_MANAGED_WEAKREF;
    } else {
        /* the runtime's own types are CPython's static builtin types */
        f |= _Py_TPFLAGS_STATIC_BUILTIN;
    }
    if (QP_CAPI_QSLOT(t, QP_CAPI_QOFF(tp_call)) == (void *)qp_capi_fwd_call)
        f |= lb->tp_flags & Py_TPFLAGS_HAVE_VECTORCALL;
    return f;
}

/* tp_flags of the mirror of a runtime type.  Py_TPFLAGS_MANAGED_DICT is
   CPython's for heap types whose __dict__ was added by a class statement
   (type_new_descriptors, inherited by subclasses); static builtin types
   with a __dict__ (BaseException, module, function) and their subclasses
   use tp_dictoffset instead.  Managed dicts have inline values unless the
   layout has items (type_ready_managed_dict). */
static unsigned long qp_capi_mirror_flags(QPType *t)
{
    unsigned long f = qp_capi_flags_from_qp(t, qp_capi_mirror_c_bits(t));
    QPType *b;

    if (f & Py_TPFLAGS_MANAGED_DICT) {
        BOOL managed = (t->flags & QP_TPFLAGS_HEAPTYPE) != 0 &&
                       qp_capi_layout_base(t)->tp_dictoffset == 0;
        for (b = t; managed && b; b = b->base) {
            if (!(b->flags & QP_TPFLAGS_HEAPTYPE) && (b->flags & QP_TPFLAGS_MANAGED_DICT))
                managed = FALSE;
        }
        if (!managed)
            f &= ~Py_TPFLAGS_MANAGED_DICT;
        else if (qp_capi_layout_base(t)->tp_itemsize == 0)
            f |= Py_TPFLAGS_INLINE_VALUES;
    }
    return f | Py_TPFLAGS_READY;
}

static size_t qp_capi_type_kind_size(JSContext *ctx, JSValueConst v)
{
    return sizeof(PyHeapTypeObject) - sizeof(PyObject);
}

static int qp_capi_type_kind_init(JSContext *ctx, PyObject *op, JSValueConst v)
{
    PyHeapTypeObject *ht = (PyHeapTypeObject *)op;
    PyTypeObject *tp = &ht->ht_type;
    QPType *t = qp_type_from_value(v);
    QPCapiTypeInfo *ti;
    const char *s;
    JSValue name;

    if (!t) {
        qp_raise_system_error(ctx, "type mirror of a non-type");
        return -1;
    }
    ti = qp_capi_tinfo_new(tp, QP_CAPI_TO_MIRROR);
    if (!ti)
        return -1;
    ti->qt = t;
    tp->tp_cache = (PyObject *)t;
    QP_CAPI_HEAD(op)->flags |= QP_CAPI_F_PERSIST;
    /* names */
    if (t->tp_name && !(t->flags & QP_TPFLAGS_HEAPTYPE)) {
        ti->name_buf = qp_capi_strdup(t->tp_name);
    } else {
        s = JS_AtomToCString(ctx, t->name);
        if (!s)
            return -1;
        ti->name_buf = qp_capi_strdup(s);
        JS_FreeCString(ctx, s);
    }
    if (!ti->name_buf)
        return -1;
    tp->tp_name = ti->name_buf;
    name = qp_str_from_atom(ctx, t->name);
    ht->ht_name = qp_capi_steal(ctx, name);
    if (!ht->ht_name)
        return -1;
    if (JS_IsString(t->qualname)) {
        ht->ht_qualname = qp_capi_new_ref(ctx, t->qualname);
        if (!ht->ht_qualname)
            return -1;
    } else {
        ht->ht_qualname = Py_NewRef(ht->ht_name);
    }
    /* layout, flags, slots */
    tp->tp_flags = qp_capi_mirror_flags(t);
    qp_capi_mirror_fill_slots(tp, t);
    tp->tp_version_tag = t->version_tag;
    return qp_capi_type_sync_tuples(ctx, tp, t);
}

/* refresh: flags and slots (after dunder assignment, __bases__, ...) */
static void qp_capi_type_kind_refresh(JSContext *ctx, PyObject *op)
{
    PyTypeObject *tp = (PyTypeObject *)op;
    QPCapiTypeInfo *ti = qp_capi_tinfo(tp);
    QPType *t;
    if (!ti || !ti->qt)
        return;
    t = ti->qt;
    if (ti->origin == QP_CAPI_TO_MIRROR) {
        tp->tp_flags = qp_capi_mirror_flags(t);
        qp_capi_mirror_fill_slots(tp, t);
    } else {
        tp->tp_flags = qp_capi_flags_from_qp(t, tp->tp_flags) |
            (tp->tp_flags & (Py_TPFLAGS_READY | Py_TPFLAGS_READYING));
    }
    tp->tp_version_tag = t->version_tag;
    if (qp_capi_type_sync_tuples(ctx, tp, t) < 0)
        PyErr_Clear();
}

static int qp_capi_type_kind_traverse(PyObject *op, visitproc visit, void *arg)
{
    PyHeapTypeObject *ht = (PyHeapTypeObject *)op;
    PyTypeObject *tp = &ht->ht_type;
    Py_VISIT(tp->tp_dict);
    Py_VISIT(tp->tp_bases);
    Py_VISIT(tp->tp_mro);
    Py_VISIT((PyObject *)tp->tp_base);
    Py_VISIT(ht->ht_name);
    Py_VISIT(ht->ht_qualname);
    Py_VISIT(ht->ht_slots);
    Py_VISIT(ht->ht_module);
    return 0;
}

static void qp_capi_type_kind_clear(JSRuntime *rt, PyObject *op)
{
    PyHeapTypeObject *ht = (PyHeapTypeObject *)op;
    PyTypeObject *tp = &ht->ht_type;
    PyObject *base = (PyObject *)tp->tp_base;
    tp->tp_base = NULL;
    Py_CLEAR(tp->tp_dict);
    Py_CLEAR(tp->tp_bases);
    Py_CLEAR(tp->tp_mro);
    Py_XDECREF(base);
    Py_CLEAR(ht->ht_name);
    Py_CLEAR(ht->ht_qualname);
    Py_CLEAR(ht->ht_slots);
    Py_CLEAR(ht->ht_module);
    if (ht->_ht_tpname) {
        if (tp->tp_name == ht->_ht_tpname)
            tp->tp_name = "?";
        qp_sys_free(ht->_ht_tpname);
        ht->_ht_tpname = NULL;
    }
    qp_capi_tinfo_free(tp);
}

/* clear for the cycle collector: tp_base, the name and the type record stay
   until dealloc, as with CPython's type_clear (the tp_clear of a metaclass
   instance reads PyObject_GetTypeData(self, metaclass)) */
static void qp_capi_type_kind_gc_clear(JSRuntime *rt, PyObject *op)
{
    PyHeapTypeObject *ht = (PyHeapTypeObject *)op;
    PyTypeObject *tp = &ht->ht_type;
    Py_CLEAR(tp->tp_dict);
    Py_CLEAR(tp->tp_bases);
    Py_CLEAR(tp->tp_mro);
    Py_CLEAR(ht->ht_name);
    Py_CLEAR(ht->ht_qualname);
    Py_CLEAR(ht->ht_slots);
    Py_CLEAR(ht->ht_module);
}

/* to_js of a C-primary TYPE object: heap types made by PyType_FromSpec* or
   allocated with tp_alloc and readied by PyType_Ready */
static JSValue qp_capi_heap_type_create(JSContext *ctx, PyTypeObject *type);

static JSValue qp_capi_type_kind_to_js(JSContext *ctx, PyObject *op)
{
    QPCapiTypeInfo *ti = qp_capi_tinfo((PyTypeObject *)op);
    if (!ti || ti->origin != QP_CAPI_TO_CHEAP)
        return qp_raise_system_error(ctx, "type object '%s' was not created by PyType_FromSpec "
                                     "or readied by PyType_Ready",
                                     ((PyTypeObject *)op)->tp_name);
    return qp_capi_heap_type_create(ctx, (PyTypeObject *)op);
}

/* ---- QPType <-> PyTypeObject -------------------------------------------- */

static QPType *qp_capi_type_qp(PyTypeObject *tp)
{
    QPCapiTypeInfo *ti;
    if (!tp)
        return NULL;
    ti = qp_capi_tinfo(tp);
    if (ti && ti->qt)
        return ti->qt;
    if (!(tp->tp_flags & Py_TPFLAGS_READY) && !(tp->tp_flags & Py_TPFLAGS_HEAPTYPE) &&
        PyType_Ready(tp) == 0) {
        ti = qp_capi_tinfo(tp);
        if (ti && ti->qt)
            return ti->qt;
    }
    if (qp_capi.ctx && !JS_HasException(qp_capi.ctx))
        PyErr_Format(PyExc_SystemError, "type '%s' is not bound to the runtime", tp->tp_name);
    return NULL;
}

/* Borrowed PyTypeObject of t; the mirror of a runtime type without a
   static/C object is created once and kept alive by a self pin until its
   type object is collected. */
static PyTypeObject *qp_capi_type_from_qp(JSContext *ctx, QPType *t)
{
    PyTypeObject *tp = qp_capi_type_map_get(t), *meta;
    PyObject *op;
    if (tp)
        return tp;
    op = qp_capi_lookup(QP_TYPE_VALUE(t));
    if (op)
        return (PyTypeObject *)op;
    meta = qp_capi_type_from_qp(ctx, qp_type_of(ctx, QP_TYPE_VALUE(t)));
    if (!meta)
        return NULL;
    op = qp_capi_new_mirror(ctx, QP_TYPE_VALUE(t), QP_CAPI_K_TYPE, meta);
    if (!op)
        return NULL;
    qp_capi_pin_set(op, QP_CAPI_PIN_NAMED(QP_CAPI_ROLE__COUNT), op);
    return (PyTypeObject *)op;
}

static PyTypeObject *qp_capi_type_of(JSContext *ctx, JSValueConst v)
{
    return qp_capi_type_from_qp(ctx, qp_type_of(ctx, v));
}

/* ---- readying stack (layout queries before a C type is registered) ----- */

typedef struct QPCapiReadying {
    struct QPCapiReadying *next;
    QPCapiTypeInfo *ti;
} QPCapiReadying;

static QPCapiReadying *qp_capi_readying;

static PyTypeObject *qp_capi_type_by_qp_any(QPType *t)
{
    QPCapiReadying *r;
    PyTypeObject *tp = qp_capi_type_map_get(t);
    if (tp)
        return tp;
    for (r = qp_capi_readying; r; r = r->next)
        if (t->spec == &r->ti->spec)
            return r->ti->tp;
    return NULL;
}

static void qp_capi_layout_of(QPType *t, Py_ssize_t *bs, Py_ssize_t *is)
{
    for (; t; t = t->base) {
        PyTypeObject *tp = qp_capi_type_by_qp_any(t);
        if (tp) {
            *bs = tp->tp_basicsize;
            *is = tp->tp_itemsize;
            return;
        }
        if (t->instance_class_id != JS_CLASS_PY_NATIVE)
            break;
    }
    *bs = sizeof(PyObject);
    *is = 0;
}

/* R4 hook (object.h): same C layout of two native types */
static BOOL qp_capi_native_same_layout(QPType *a, QPType *b)
{
    Py_ssize_t ab, ai, bb, bi;
    qp_capi_layout_of(a, &ab, &ai);
    qp_capi_layout_of(b, &bb, &bi);
    return ab == bb && ai == bi;
}

/* R5 hook (object.h): called by qp_type_modified */
static PyTypeObject *qp_capi_type_watched(PyTypeObject *tp);

static void qp_capi_type_modified(JSContext *ctx, QPType *t)
{
    PyTypeObject *tp;
    if (!qp_capi.ctx || ctx != qp_capi.ctx || qp_capi.initialized != 2)
        return;
    tp = qp_capi_type_map_get(t);
    if (!tp) {
        PyObject *op = qp_capi_lookup(QP_TYPE_VALUE(t));
        if (!op || !QP_CAPI_HAS_HEAD(op) || QP_CAPI_HEAD(op)->kind != QP_CAPI_K_TYPE)
            return;
        tp = (PyTypeObject *)op;
    }
    if (qp_capi_tinfo(tp))
        qp_capi_type_kind_refresh(ctx, (PyObject *)tp);
    /* type_modified_unlocked: the watchers see the first modification
       after the version tag was assigned (a lookup), so that consecutive
       modifications are reported once */
    if (t->version_tag != 0)
        qp_capi_type_watched(tp);
}

/* ========================================================================= */
/* Extension instances (kind NATIVE)                                          */
/* ========================================================================= */

/* Slot at tp_dictoffset (inside the C instance; a negative offset counts
   from the end of the instance, _PyObject_ComputedDictPointer) of
   extension object op, NULL if its type has none. */
static PyObject **qp_capi_native_dictptr(PyObject *op)
{
    PyTypeObject *tp = Py_TYPE(op);
    Py_ssize_t off = tp->tp_dictoffset, size = tp->tp_basicsize;

    if (off < 0) {
        if (tp->tp_itemsize) {
            Py_ssize_t n = Py_SIZE(op);
            size = (Py_ssize_t)_PyObject_VAR_SIZE(tp, n < 0 ? -n : n);
        }
        off += size;
    }
    if (off <= 0 || off + (Py_ssize_t)sizeof(PyObject *) > size)
        return NULL;
    return (PyObject **)((char *)op + off);
}

static size_t qp_capi_native_kind_size(JSContext *ctx, JSValueConst v)
{
    PyTypeObject *tp = qp_capi_type_of(ctx, v);
    if (!tp || tp->tp_basicsize <= (Py_ssize_t)sizeof(PyObject))
        return 0;
    return (size_t)tp->tp_basicsize - sizeof(PyObject);
}

static JSValue qp_capi_native_kind_to_js(JSContext *ctx, PyObject *op)
{
    QPType *t = qp_capi_type_qp(Py_TYPE(op));
    JSValue v;
    PyObject **dp;
    if (!t)
        return JS_EXCEPTION;
    if (t->instance_class_id != JS_CLASS_PY_NATIVE)
        return qp_raise_system_error(ctx, "'%N' instances are not extension objects", t);
    v = JS_NewObjectProtoClass(ctx, QP_TYPE_VALUE(t), JS_CLASS_PY_NATIVE);
    if (JS_IsException(v))
        return v;
    JS_VALUE_GET_OBJ(v)->u.opaque = op;
    /* the slot at tp_dictoffset holds the instance __dict__ (C code reads
       and replaces it, e.g. partial's __reduce__/__setstate__): the object
       is in dict mode from the start and qp_capi_native_dict_sync follows
       the slot.  A dict the C code already stored there (numpy's ufunc sets
       ufunc->dict in its constructor) is adopted at once. */
    dp = qp_capi_native_dictptr(op);
    if (dp) {
        JS_VALUE_GET_OBJ(v)->py_dict_mode = 1;
        if (*dp && PyDict_Check(*dp)) {
            JSValue d = qp_capi_value(ctx, *dp);
            if (JS_IsException(d) || qp_obj_adopt_dict(ctx, JS_VALUE_GET_OBJ(v), d) < 0) {
                JS_VALUE_GET_OBJ(v)->u.opaque = NULL;
                JS_FreeValue(ctx, v);
                return JS_EXCEPTION;
            }
        }
    }
    return v;
}

/* The runtime set the instance __dict__ of a C extension object
   (JS_CLASS_PY_NATIVE p) to d: C code that reads the slot at tp_dictoffset
   sees it too (CPython's _PyObject_GetDictPtr storage). */
static void qp_capi_native_dict_made(JSContext *ctx, JSObject *p, JSValueConst d)
{
    PyObject *op = p->u.opaque, **dp, *nd;
    if (!op || !(dp = qp_capi_native_dictptr(op)))
        return;
    if (*dp) {
        JSValue cur = qp_capi_value(ctx, *dp);
        if (JS_IsException(cur))
            JS_FreeValue(ctx, JS_GetException(ctx));
        else if (JS_VALUE_GET_PTR(cur) == JS_VALUE_GET_PTR(d))
            return;
    }
    nd = qp_capi_new_ref(ctx, d);
    if (!nd) {
        JS_FreeValue(ctx, JS_GetException(ctx));
        return;
    }
    Py_XSETREF(*dp, nd);
}

/* Borrowed instance __dict__ of extension object p (JS_CLASS_PY_NATIVE),
   QP_NULL if none.  When its type has a tp_dictoffset slot, the slot is
   the source of truth: a dict the C code stored there replaces the
   runtime's, and a slot the C code cleared reads as a new empty dict
   (PyObject_GenericGetDict). */
static JSValue qp_capi_native_dict_sync(JSContext *ctx, JSObject *p)
{
    JSShapeProperty *prs;
    JSProperty *pr;
    PyObject *op = p->u.opaque, **dp;
    JSValue cur, d;

    prs = find_own_property(&pr, p, QP_STATE(ctx)->atom_dict_slot);
    cur = prs ? pr->u.value : QP_NULL;
    if (!op || !(dp = qp_capi_native_dictptr(op)))
        return cur;
    if (!*dp) {
        if (!prs)
            return QP_NULL;
        d = qp_dict_new(ctx);
        if (JS_IsException(d)) {
            JS_FreeValue(ctx, JS_GetException(ctx));
            return cur;
        }
        *dp = qp_capi_new_ref(ctx, d);
        if (!*dp) {
            JS_FreeValue(ctx, JS_GetException(ctx));
            JS_FreeValue(ctx, d);
            return cur;
        }
        set_value(ctx, &pr->u.value, d);
        return d;
    }
    if (!PyDict_Check(*dp))
        return cur;
    d = qp_capi_value(ctx, *dp);
    if (JS_IsException(d)) {
        JS_FreeValue(ctx, JS_GetException(ctx));
        return cur;
    }
    if (prs) {
        if (JS_VALUE_GET_PTR(d) != JS_VALUE_GET_PTR(cur))
            set_value(ctx, &pr->u.value, JS_DupValue(ctx, d));
        return d;
    }
    pr = add_property(ctx, p, QP_STATE(ctx)->atom_dict_slot, JS_PROP_WRITABLE);
    if (!pr) {
        JS_FreeValue(ctx, JS_GetException(ctx));
        return QP_NULL;
    }
    pr->u.value = JS_DupValue(ctx, d);
    p->py_dict_mode = 1;
    return d;
}

/* __class__ assignment: ob_type follows the JS prototype */
/* The C object of a J the runtime created (object.__new__ of a C type,
   allocated at its first crossing): as for the instances C creates
   (qp_capi_native_kind_to_js), the slot at tp_dictoffset holds the
   instance __dict__ from now on.  Attributes already set move into the
   dict; without any, the dict is made by the first one. */
static int qp_capi_native_kind_init(JSContext *ctx, PyObject *op, JSValueConst v)
{
    JSObject *p = JS_VALUE_GET_OBJ(v);
    JSShapeProperty *prs;
    JSValue d;
    int i;

    if (!qp_capi_native_dictptr(op))
        return 0;
    d = qp_obj_dict_get(ctx, p);
    if (!QP_IS_NULL(d)) {
        qp_capi_native_dict_made(ctx, p, d);
        return 0;
    }
    for (i = 0, prs = get_shape_prop(p->shape); i < p->shape->prop_count; i++, prs++) {
        if (qp_shape_prop_is_attr(ctx, prs))
            return JS_IsException(qp_obj_materialize_dict(ctx, p)) ? -1 : 0;
    }
    p->py_dict_mode = 1;
    return 0;
}

static void qp_capi_native_kind_refresh(JSContext *ctx, PyObject *op)
{
    QPCapiHead *h = QP_CAPI_HEAD(op);
    PyTypeObject *nt, *old = Py_TYPE(op);
    QPType *t;
    if (QP_IS_NULL(h->v) || (h->flags & QP_CAPI_F_ORPHAN))
        return;
    t = qp_type_of(ctx, h->v);
    if ((QPType *)old->tp_cache == t)
        return;
    nt = qp_capi_type_from_qp(ctx, t);
    if (!nt) {
        PyErr_Clear();
        return;
    }
    if (QP_CAPI_HAS_HEAD(nt))
        Py_INCREF(nt);
    Py_SET_TYPE(op, nt);
    if (QP_CAPI_HAS_HEAD(old))
        Py_DECREF(old);
}

/* ========================================================================= */
/* C-defined types: PyType_Ready, PyType_FromSpec & co                        */
/* ========================================================================= */

/* ---- C-side inheritance (CPython inherit_special / inherit_slots) -------- */

static void qp_capi_inherit_special(PyTypeObject *type, PyTypeObject *base)
{
    if (!(type->tp_flags & Py_TPFLAGS_HAVE_GC) && (base->tp_flags & Py_TPFLAGS_HAVE_GC) &&
        !type->tp_traverse && !type->tp_clear) {
        type->tp_flags |= Py_TPFLAGS_HAVE_GC;
        type->tp_traverse = base->tp_traverse;
        type->tp_clear = base->tp_clear;
    }
    type->tp_flags |= base->tp_flags & Py_TPFLAGS_PREHEADER;
    if (type->tp_basicsize == 0)
        type->tp_basicsize = base->tp_basicsize;
    if (!type->tp_itemsize)
        type->tp_itemsize = base->tp_itemsize;
    if (!type->tp_weaklistoffset)
        type->tp_weaklistoffset = base->tp_weaklistoffset;
    if (!type->tp_dictoffset)
        type->tp_dictoffset = base->tp_dictoffset;
    type->tp_flags |= base->tp_flags & (_Py_TPFLAGS_MATCH_SELF | Py_TPFLAGS_ITEMS_AT_END);
}

#define QP_CAPI_COPY(F) do { if (!type->F && base->F) type->F = base->F; } while (0)
#define QP_CAPI_COPY_SUITE(S, F) do { if (!type->S->F && base->S->F) type->S->F = base->S->F; } while (0)

static int qp_capi_inherit_slots(JSContext *ctx, QPType *qt, PyTypeObject *type, PyTypeObject *base)
{
    if (type->tp_as_number && base->tp_as_number) {
        QP_CAPI_COPY_SUITE(tp_as_number, nb_add); QP_CAPI_COPY_SUITE(tp_as_number, nb_subtract);
        QP_CAPI_COPY_SUITE(tp_as_number, nb_multiply); QP_CAPI_COPY_SUITE(tp_as_number, nb_remainder);
        QP_CAPI_COPY_SUITE(tp_as_number, nb_divmod); QP_CAPI_COPY_SUITE(tp_as_number, nb_power);
        QP_CAPI_COPY_SUITE(tp_as_number, nb_negative); QP_CAPI_COPY_SUITE(tp_as_number, nb_positive);
        QP_CAPI_COPY_SUITE(tp_as_number, nb_absolute); QP_CAPI_COPY_SUITE(tp_as_number, nb_bool);
        QP_CAPI_COPY_SUITE(tp_as_number, nb_invert); QP_CAPI_COPY_SUITE(tp_as_number, nb_lshift);
        QP_CAPI_COPY_SUITE(tp_as_number, nb_rshift); QP_CAPI_COPY_SUITE(tp_as_number, nb_and);
        QP_CAPI_COPY_SUITE(tp_as_number, nb_xor); QP_CAPI_COPY_SUITE(tp_as_number, nb_or);
        QP_CAPI_COPY_SUITE(tp_as_number, nb_int); QP_CAPI_COPY_SUITE(tp_as_number, nb_float);
        QP_CAPI_COPY_SUITE(tp_as_number, nb_inplace_add);
        QP_CAPI_COPY_SUITE(tp_as_number, nb_inplace_subtract);
        QP_CAPI_COPY_SUITE(tp_as_number, nb_inplace_multiply);
        QP_CAPI_COPY_SUITE(tp_as_number, nb_inplace_remainder);
        QP_CAPI_COPY_SUITE(tp_as_number, nb_inplace_power);
        QP_CAPI_COPY_SUITE(tp_as_number, nb_inplace_lshift);
        QP_CAPI_COPY_SUITE(tp_as_number, nb_inplace_rshift);
        QP_CAPI_COPY_SUITE(tp_as_number, nb_inplace_and);
        QP_CAPI_COPY_SUITE(tp_as_number, nb_inplace_xor);
        QP_CAPI_COPY_SUITE(tp_as_number, nb_inplace_or);
        QP_CAPI_COPY_SUITE(tp_as_number, nb_true_divide);
        QP_CAPI_COPY_SUITE(tp_as_number, nb_floor_divide);
        QP_CAPI_COPY_SUITE(tp_as_number, nb_inplace_true_divide);
        QP_CAPI_COPY_SUITE(tp_as_number, nb_inplace_floor_divide);
        QP_CAPI_COPY_SUITE(tp_as_number, nb_index);
        QP_CAPI_COPY_SUITE(tp_as_number, nb_matrix_multiply);
        QP_CAPI_COPY_SUITE(tp_as_number, nb_inplace_matrix_multiply);
    }
    if (type->tp_as_async && base->tp_as_async) {
        QP_CAPI_COPY_SUITE(tp_as_async, am_await); QP_CAPI_COPY_SUITE(tp_as_async, am_aiter);
        QP_CAPI_COPY_SUITE(tp_as_async, am_anext); QP_CAPI_COPY_SUITE(tp_as_async, am_send);
    }
    if (type->tp_as_sequence && base->tp_as_sequence) {
        QP_CAPI_COPY_SUITE(tp_as_sequence, sq_length); QP_CAPI_COPY_SUITE(tp_as_sequence, sq_concat);
        QP_CAPI_COPY_SUITE(tp_as_sequence, sq_repeat); QP_CAPI_COPY_SUITE(tp_as_sequence, sq_item);
        QP_CAPI_COPY_SUITE(tp_as_sequence, sq_ass_item);
        QP_CAPI_COPY_SUITE(tp_as_sequence, sq_contains);
        QP_CAPI_COPY_SUITE(tp_as_sequence, sq_inplace_concat);
        QP_CAPI_COPY_SUITE(tp_as_sequence, sq_inplace_repeat);
    }
    if (type->tp_as_mapping && base->tp_as_mapping) {
        QP_CAPI_COPY_SUITE(tp_as_mapping, mp_length); QP_CAPI_COPY_SUITE(tp_as_mapping, mp_subscript);
        QP_CAPI_COPY_SUITE(tp_as_mapping, mp_ass_subscript);
    }
    if (type->tp_as_buffer && base->tp_as_buffer) {
        QP_CAPI_COPY_SUITE(tp_as_buffer, bf_getbuffer);
        QP_CAPI_COPY_SUITE(tp_as_buffer, bf_releasebuffer);
    }
    QP_CAPI_COPY(tp_dealloc);
    if (!type->tp_getattr && !type->tp_getattro) {
        type->tp_getattr = base->tp_getattr;
        type->tp_getattro = base->tp_getattro;
    }
    if (!type->tp_setattr && !type->tp_setattro) {
        type->tp_setattr = base->tp_setattr;
        type->tp_setattro = base->tp_setattro;
    }
    QP_CAPI_COPY(tp_repr);
    QP_CAPI_COPY(tp_vectorcall_offset);
    if (!type->tp_call && (base->tp_flags & Py_TPFLAGS_HAVE_VECTORCALL))
        type->tp_flags |= Py_TPFLAGS_HAVE_VECTORCALL;
    QP_CAPI_COPY(tp_call);
    QP_CAPI_COPY(tp_str);
    if (!type->tp_richcompare && !type->tp_hash) {
        JSValue tmp;
        int r = qp_dict_get_item_atom(ctx, qt->dict, QP_ATOM(ctx, __eq__), &tmp);
        if (r > 0)
            JS_FreeValue(ctx, tmp);
        if (r == 0) {
            r = qp_dict_get_item_atom(ctx, qt->dict, QP_ATOM(ctx, __hash__), &tmp);
            if (r > 0)
                JS_FreeValue(ctx, tmp);
        }
        if (r < 0)
            return -1;
        if (r == 0) {
            type->tp_richcompare = base->tp_richcompare;
            type->tp_hash = base->tp_hash;
        }
    }
    QP_CAPI_COPY(tp_iter);
    QP_CAPI_COPY(tp_iternext);
    QP_CAPI_COPY(tp_descr_get);
    if (base->tp_descr_get && type->tp_descr_get == base->tp_descr_get &&
        (type->tp_flags & Py_TPFLAGS_IMMUTABLETYPE) &&
        (base->tp_flags & Py_TPFLAGS_METHOD_DESCRIPTOR))
        type->tp_flags |= Py_TPFLAGS_METHOD_DESCRIPTOR;
    QP_CAPI_COPY(tp_descr_set);
    QP_CAPI_COPY(tp_dictoffset);
    QP_CAPI_COPY(tp_init);
    QP_CAPI_COPY(tp_alloc);
    QP_CAPI_COPY(tp_is_gc);
    QP_CAPI_COPY(tp_finalize);
    if ((type->tp_flags & Py_TPFLAGS_HAVE_GC) == (base->tp_flags & Py_TPFLAGS_HAVE_GC))
        QP_CAPI_COPY(tp_free);
    else if ((type->tp_flags & Py_TPFLAGS_HAVE_GC) && !type->tp_free && base->tp_free == PyObject_Free)
        type->tp_free = PyObject_GC_Del;
    return 0;
}

#undef QP_CAPI_COPY
#undef QP_CAPI_COPY_SUITE

/* type_ready_inherit + set_new + set_hash on the C side, after the runtime
   type exists (tp_mro set). */
static int qp_capi_c_inherit(JSContext *ctx, QPCapiTypeInfo *ti)
{
    PyTypeObject *type = ti->tp, *base = type->tp_base;
    Py_ssize_t i, n;

    if (!base)
        return 0;
    qp_capi_inherit_special(type, base);
    if (!(type->tp_flags & Py_TPFLAGS_HEAPTYPE)) {
        if (!type->tp_as_async) type->tp_as_async = base->tp_as_async;
        if (!type->tp_as_number) type->tp_as_number = base->tp_as_number;
        if (!type->tp_as_sequence) type->tp_as_sequence = base->tp_as_sequence;
        if (!type->tp_as_mapping) type->tp_as_mapping = base->tp_as_mapping;
        if (!type->tp_as_buffer) type->tp_as_buffer = base->tp_as_buffer;
    }
    n = type->tp_mro ? PyTuple_GET_SIZE(type->tp_mro) : 0;
    for (i = 1; i < n; i++) {
        PyObject *b = PyTuple_GET_ITEM(type->tp_mro, i);
        if (PyType_Check(b) && qp_capi_inherit_slots(ctx, ti->qt, type, (PyTypeObject *)b) < 0)
            return -1;
    }
    /* type_ready_set_new */
    if (type->tp_flags & Py_TPFLAGS_DISALLOW_INSTANTIATION)
        type->tp_new = NULL;
    else if (!type->tp_new)
        type->tp_new = base->tp_new;
    /* type_ready_set_hash */
    if (!type->tp_hash && ti->qt->slots.tp_hash == qp_hash_not_implemented)
        type->tp_hash = PyObject_HashNotImplemented;
    if (!type->tp_alloc)
        type->tp_alloc = PyType_GenericAlloc;
    if (!type->tp_free)
        type->tp_free = (type->tp_flags & Py_TPFLAGS_HAVE_GC) ? PyObject_GC_Del : PyObject_Free;
    return 0;
}

/* ---- the QP spec of a C-defined type ------------------------------------- */

/* __del__ of C types with tp_finalize / tp_del (run by the runtime's
   pending-finalizer queue, PEP 442 order) */
static JSValue qp_capi_del_meth(JSContext *ctx, JSValueConst self, JSValueConst *args, int nargs,
                                JSValueConst kwnames, int magic)
{
    PyObject *x = qp_capi_new_ref(ctx, self);
    PyTypeObject *tp;
    QPCapiHead *h;
    if (!x)
        return JS_EXCEPTION;
    h = QP_CAPI_HAS_HEAD(x) ? QP_CAPI_HEAD(x) : NULL;
    tp = Py_TYPE(x);
    if (tp->tp_finalize && !(h && (h->flags & QP_CAPI_F_FINALIZED))) {
        if (h)
            h->flags |= QP_CAPI_F_FINALIZED;
        tp->tp_finalize(x);
    }
    if (tp->tp_del && !JS_HasException(ctx))
        tp->tp_del(x);
    Py_DECREF(x);
    return JS_HasException(ctx) ? JS_EXCEPTION : JS_UNDEFINED;
}

static const QPMethodDef qp_capi_del_methoddef = {
    "__del__", qp_capi_del_meth, QP_METH_NOARGS, 0, NULL
};

/* QP instance class of a C type: natives over object, else the builtin's */
static uint16_t qp_capi_instance_class(PyTypeObject *base)
{
    QPType *b = qp_capi_type_qp(base);
    if (!b || b->instance_class_id == JS_CLASS_PY_OBJECT)
        return JS_CLASS_PY_NATIVE;
    return 0;   /* inherit (PY_NATIVE of a C base, PY_DICT of dict, ...) */
}

static const char *qp_capi_type_doc(QPType *t)
{
    PyTypeObject *tp;
    QPCapiTypeInfo *ti;
    if (!t->spec)
        return NULL;
    if ((tp = qp_capi_type_map_get(t)) && (ti = qp_capi_tinfo(tp)) && &ti->spec == t->spec)
        return tp->tp_doc;
    return t->spec->doc;
}

/* Build ti->spec from the type's own (not yet inherited) C slots. */
static int qp_capi_build_spec(JSContext *ctx, QPCapiTypeInfo *ti)
{
    PyTypeObject *tp = ti->tp;
    QPTypeSpec *s = &ti->spec;
    size_t i, n;

    memset(s, 0, sizeof(*s));
    s->name = tp->tp_name;
    s->doc = tp->tp_doc;
    s->flags = qp_capi_flags_to_qp(tp) | QP_TPFLAGS_PRIV_CAPI;
    s->base = -1;
    s->exc_base = -1;
    s->instance_class_id = qp_capi_instance_class(tp->tp_base ? tp->tp_base : &PyBaseObject_Type);
    for (i = 0; i < countof(qp_capi_slot_map); i++) {
        const QPCapiSlotMapEntry *e = &qp_capi_slot_map[i];
        void *cf = qp_capi_cslot_get(tp, e), *qf;
        if (!cf && e->qoff == QP_CAPI_QOFF(tp_getattro))
            cf = (void *)tp->tp_getattr;
        if (!cf && e->qoff == QP_CAPI_QOFF(tp_setattro))
            cf = (void *)tp->tp_setattr;
        if (!cf)
            continue;
        qf = qp_capi_known_c2q(e->qoff, cf);
        *(void **)((uint8_t *)&s->slots + e->qoff) = qf ? qf : e->fwd;
    }
    if (tp->tp_vectorcall)
        s->slots.tp_vectorcall = qp_capi_fwd_type_vectorcall;
    if (tp->tp_as_buffer && tp->tp_as_buffer->bf_getbuffer) {
        s->slots.bf.bf_getbuffer = qp_capi_bf_getbuffer_fwd;
        s->slots.bf.bf_releasebuffer = qp_capi_bf_releasebuffer_fwd;
    }
    if (tp->tp_methods) {
        for (n = 0; tp->tp_methods[n].ml_name; n++)
            ;
        ti->methods = qp_sys_calloc(n + 1, sizeof(QPMethodDef));
        if (!ti->methods) {
            PyErr_NoMemory();
            return -1;
        }
        for (i = 0; i < n; i++) {
            const QPMethodDef *q = qp_capi_method_def(ctx, &tp->tp_methods[i], tp);
            if (!q)
                return -1;
            ti->methods[i] = *q;
        }
        s->methods = ti->methods;
    }
    if (tp->tp_getset) {
        for (n = 0; tp->tp_getset[n].name; n++)
            ;
        ti->getsets = qp_sys_calloc(n + 1, sizeof(QPGetSetDef));
        if (!ti->getsets) {
            PyErr_NoMemory();
            return -1;
        }
        for (i = 0; i < n; i++) {
            const QPGetSetDef *q = qp_capi_getset_def(ctx, &tp->tp_getset[i]);
            if (!q)
                return -1;
            ti->getsets[i] = *q;
        }
        s->getsets = ti->getsets;
    }
    return 0;
}

/* dict.setdefault(name, descr) on the type dict (consumes d) */
static int qp_capi_type_dict_setdefault(JSContext *ctx, QPType *t, const char *name, JSValue d)
{
    JSAtom a;
    JSValue tmp;
    int r;
    if (JS_IsException(d))
        return -1;
    a = JS_NewAtom(ctx, name);
    if (a == JS_ATOM_NULL) {
        JS_FreeValue(ctx, d);
        return -1;
    }
    /* CPython adds the members before type_dict_set_doc, which fills
       __doc__ only when missing: a __doc__ member (_tuplegetter) replaces
       the doc that qp_type_new_from_spec_bases stored */
    r = strcmp(name, "__doc__") ? qp_dict_get_item_atom(ctx, t->dict, a, &tmp) : 0;
    if (r > 0)
        JS_FreeValue(ctx, tmp);
    else if (r == 0)
        r = qp_dict_set_item_atom(ctx, t->dict, a, d);
    JS_FreeAtom(ctx, a);
    JS_FreeValue(ctx, d);
    return r < 0 ? -1 : 0;
}

/* Create the runtime type of a C-defined type (spec + bases); new
   reference to the type object. */
static JSValue qp_capi_c_type_create(JSContext *ctx, QPCapiTypeInfo *ti)
{
    PyTypeObject *tp = ti->tp;
    QPCapiReadying rd;
    QPType *meta, *t;
    JSValue bases, v, bj;
    PyTypeObject *base = tp->tp_base ? tp->tp_base : &PyBaseObject_Type;

    if (qp_capi_build_spec(ctx, ti) < 0)
        return JS_EXCEPTION;
    meta = qp_capi_type_qp(Py_TYPE(tp));
    if (!meta)
        return JS_EXCEPTION;
    if (tp->tp_bases) {
        bases = qp_capi_to_js(ctx, tp->tp_bases);
    } else {
        bj = qp_capi_value(ctx, (PyObject *)base);
        if (JS_IsException(bj))
            return JS_EXCEPTION;
        bases = qp_tuple_pack(ctx, 1, (JSValueConst *)&bj);
    }
    if (JS_IsException(bases))
        return JS_EXCEPTION;
    rd.ti = ti;
    rd.next = qp_capi_readying;
    qp_capi_readying = &rd;
    v = qp_type_new_from_spec_bases(ctx, meta, &ti->spec, bases, QP_NULL);
    qp_capi_readying = rd.next;
    JS_FreeValue(ctx, bases);
    if (JS_IsException(v))
        return v;
    t = qp_type_from_value(v);
    ti->qt = t;
    tp->tp_cache = (PyObject *)t;
    if (qp_capi_pmap_set(&qp_capi_type_pmap, t, NULL, tp) < 0) {
        JS_FreeValue(ctx, v);
        PyErr_NoMemory();
        return JS_EXCEPTION;
    }
    return v;
}

/* After the runtime type exists: members, __del__, C inheritance, flags,
   tp_dict/tp_bases/tp_mro. */
static int qp_capi_c_type_finish(JSContext *ctx, QPCapiTypeInfo *ti, int heap)
{
    PyTypeObject *tp = ti->tp;
    QPType *t = ti->qt;
    PyMemberDef *m;

    for (m = tp->tp_members; m && m->name; m++) {
        const QPGetSetDef *q;
        if (heap && (!strcmp(m->name, "__weaklistoffset__") || !strcmp(m->name, "__dictoffset__")))
            continue;
        q = qp_capi_member_def(ctx, m);
        if (!q || qp_capi_type_dict_setdefault(ctx, t, m->name, qp_descr_new_member_getset(ctx, t, q)) < 0)
            return -1;
    }
    if ((tp->tp_finalize || tp->tp_del) &&
        qp_capi_type_dict_setdefault(ctx, t, "__del__", qp_descr_new_method(ctx, t, &qp_capi_del_methoddef)) < 0)
        return -1;
    /* a tp_dict set before PyType_Ready (numpy: ufunc.__signature__) is the
       type dict in CPython, whose entries the slot/method descriptors do
       not replace (type_ready_set_dict, PyDict_SetDefault) */
    if (tp->tp_dict && PyDict_Check(tp->tp_dict) && JS_VALUE_GET_TAG(t->dict) == JS_TAG_OBJECT &&
        JS_VALUE_GET_PTR(qp_capi_value(ctx, tp->tp_dict)) != JS_VALUE_GET_PTR(t->dict)) {
        PyObject *td = qp_capi_new_ref(ctx, t->dict);
        int r = td ? PyDict_Update(td, tp->tp_dict) : -1;
        Py_XDECREF(td);
        if (r < 0)
            return -1;
    }
    qp_type_modified(ctx, t);
    if (qp_capi_type_sync_tuples(ctx, tp, t) < 0)
        return -1;
    if (qp_capi_c_inherit(ctx, ti) < 0)
        return -1;
    tp->tp_flags |= (unsigned long)t->flags & (QP_CAPI_FLAGS_SHARED & ~(unsigned long)QP_TPFLAGS_HAS_DEL);
    tp->tp_flags &= ~Py_TPFLAGS_READYING;
    tp->tp_flags |= Py_TPFLAGS_READY;
    tp->tp_version_tag = t->version_tag;
    return 0;
}

/* type_ready_preheader; HAVE_GC as inherit_special will set it from base */
static int qp_capi_type_ready_preheader(PyTypeObject *type, PyTypeObject *base)
{
    BOOL gc = (type->tp_flags & Py_TPFLAGS_HAVE_GC) ||
              (base && (base->tp_flags & Py_TPFLAGS_HAVE_GC) &&
               !type->tp_traverse && !type->tp_clear);

    if (type->tp_flags & Py_TPFLAGS_MANAGED_DICT) {
        if (type->tp_dictoffset > 0 || type->tp_dictoffset < -1) {
            PyErr_Format(PyExc_TypeError, "type %s has the Py_TPFLAGS_MANAGED_DICT flag "
                         "but tp_dictoffset is set", type->tp_name);
            return -1;
        }
        if (!gc) {
            PyErr_Format(PyExc_SystemError, "type %s has the Py_TPFLAGS_MANAGED_DICT flag "
                         "but not Py_TPFLAGS_HAVE_GC flag", type->tp_name);
            return -1;
        }
    }
    if (type->tp_flags & Py_TPFLAGS_MANAGED_WEAKREF) {
        /* CPython's MANAGED_WEAKREF_OFFSET (pycore_object.h) */
        if (type->tp_weaklistoffset != 0 &&
            type->tp_weaklistoffset != (Py_ssize_t)sizeof(PyObject *) * -4) {
            PyErr_Format(PyExc_TypeError, "type %s has the Py_TPFLAGS_MANAGED_WEAKREF flag "
                         "but tp_weaklistoffset is set", type->tp_name);
            return -1;
        }
        if (!gc) {
            PyErr_Format(PyExc_SystemError, "type %s has the Py_TPFLAGS_MANAGED_WEAKREF flag "
                         "but not Py_TPFLAGS_HAVE_GC flag", type->tp_name);
            return -1;
        }
    }
    return 0;
}

int PyType_Ready(PyTypeObject *type)
{
    JSContext *ctx = qp_capi.ctx;
    QPCapiTypeInfo *ti;
    PyTypeObject *base;
    JSValue v;
    int heap;

    if (type->tp_flags & Py_TPFLAGS_READY)
        return 0;
    if (!ctx) {
        Py_FatalError("PyType_Ready: the C API is not initialized");
        return -1;
    }
    if (qp_capi_tinfo(type)) {
        PyErr_Format(PyExc_SystemError, "type '%s' is being readied recursively", type->tp_name);
        return -1;
    }
    if (!type->tp_name) {
        PyErr_SetString(PyExc_SystemError, "Type does not define the tp_name field.");
        return -1;
    }
    base = type->tp_base;
    if (!base && type != &PyBaseObject_Type) {
        /* type_ready_set_base: a heap type owns its tp_base */
        base = &PyBaseObject_Type;
        type->tp_base = (type->tp_flags & Py_TPFLAGS_HEAPTYPE) ? (PyTypeObject *)Py_NewRef(base) : base;
    }
    if (base && !(base->tp_flags & Py_TPFLAGS_READY) && PyType_Ready(base) < 0)
        return -1;
    if (!Py_TYPE(type) && base)
        Py_SET_TYPE(type, Py_TYPE(base));
    if (!Py_TYPE(type))
        Py_SET_TYPE(type, &PyType_Type);
    if (type->tp_bases) {
        Py_ssize_t i;
        for (i = 0; i < PyTuple_GET_SIZE(type->tp_bases); i++) {
            PyObject *b = PyTuple_GET_ITEM(type->tp_bases, i);
            if (PyType_Check(b) && !(((PyTypeObject *)b)->tp_flags & Py_TPFLAGS_READY) &&
                PyType_Ready((PyTypeObject *)b) < 0)
                return -1;
        }
    }
    /* type_ready: historically, all static types were immutable (bpo-43908;
       _zoneinfo derives an immutable heap type from datetime.tzinfo) */
    if (!(type->tp_flags & Py_TPFLAGS_HEAPTYPE)) {
        type->tp_flags |= Py_TPFLAGS_IMMUTABLETYPE;
        /* a static type defined without PyObject_HEAD_INIT (designated
           initializers only, as _testcapi's RecursingInfinitelyError) has
           no head: what PyObject_HEAD_INIT would have set */
        if (!(((PyObject *)type)->ob_flags & _Py_STATICALLY_ALLOCATED_FLAG) && Py_REFCNT(type) == 0)
            ((PyObject *)type)->ob_refcnt_full = _Py_STATIC_IMMORTAL_INITIAL_REFCNT;
    }
    if (qp_capi_type_ready_preheader(type, base) < 0)
        return -1;
    /* A PyHeapTypeObject allocated with tp_alloc of a metatype and filled in
       by hand (pybind11's metaclass, object base and classes) is a heap type
       like PyType_FromSpec's: its runtime type is made by the TYPE kind
       to_js when it first crosses. */
    heap = (type->tp_flags & Py_TPFLAGS_HEAPTYPE) && QP_CAPI_HAS_HEAD((PyObject *)type);
    type->tp_flags |= Py_TPFLAGS_READYING;
    ti = qp_capi_tinfo_new(type, heap ? QP_CAPI_TO_CHEAP : QP_CAPI_TO_CSTATIC);
    if (!ti)
        goto fail;
    if (heap) {
        if (!type->tp_dealloc)
            type->tp_dealloc = qp_capi_subtype_dealloc;
        if (JS_IsException(qp_capi_value(ctx, (PyObject *)type)))
            goto fail;
    } else {
        v = qp_capi_c_type_create(ctx, ti);
        if (JS_IsException(v))
            goto fail;
        ti->j = v;
    }
    if (qp_capi_c_type_finish(ctx, ti, heap) < 0)
        goto fail;
    return 0;
fail:
    type->tp_flags &= ~Py_TPFLAGS_READYING;
    if (ti && !(heap && ti->qt)) {
        JS_FreeValue(ctx, ti->j);
        ti->j = QP_NULL;
        qp_capi_tinfo_free(type);
    }
    return -1;
}

/* _PyStaticType_InitForExtension (used by _datetime): PyType_Ready of a
   static type of an extension, kept immortal */
int _PyStaticType_InitForExtension(PyInterpreterState *interp, PyTypeObject *self)
{
    (void)interp;
    return PyType_Ready(self);
}

/* ---- heap types ---------------------------------------------------------- */

static JSValue qp_capi_heap_type_create(JSContext *ctx, PyTypeObject *type)
{
    QPCapiTypeInfo *ti = qp_capi_tinfo(type);
    if (!ti)
        return qp_raise_system_error(ctx, "heap type record missing");
    return qp_capi_c_type_create(ctx, ti);
}

#define QP_CAPI_ALIGN_UP(n) (((n) + (Py_ssize_t)(sizeof(void *) - 1)) & ~(Py_ssize_t)(sizeof(void *) - 1))

/* special_offset_from_member */
static int qp_capi_special_offset(const PyMemberDef *memb, Py_ssize_t type_data_offset, Py_ssize_t *dest)
{
    if (!memb) {
        *dest = 0;
        return 0;
    }
    if (memb->type != Py_T_PYSSIZET) {
        PyErr_Format(PyExc_SystemError, "type of %s must be Py_T_PYSSIZET", memb->name);
        return -1;
    }
    if (memb->flags == Py_READONLY) {
        *dest = memb->offset;
        return 0;
    }
    if (memb->flags == (Py_READONLY | Py_RELATIVE_OFFSET)) {
        *dest = memb->offset + type_data_offset;
        return 0;
    }
    PyErr_Format(PyExc_SystemError,
                 "flags for %s must be Py_READONLY or (Py_READONLY | Py_RELATIVE_OFFSET)", memb->name);
    return -1;
}

/* check_immutable_bases */
static int qp_capi_check_immutable_bases(const char *type_name, PyObject *bases)
{
    Py_ssize_t i;
    for (i = 0; i < PyTuple_GET_SIZE(bases); i++) {
        PyTypeObject *b = (PyTypeObject *)PyTuple_GET_ITEM(bases, i);
        if (!b)
            return -1;
        if (!PyType_HasFeature(b, Py_TPFLAGS_IMMUTABLETYPE)) {
            PyErr_Format(PyExc_TypeError,
                         "Creating immutable type %s from mutable base %N", type_name, b);
            return -1;
        }
    }
    return 0;
}

/* Unit h private functions (python/typeobject.c, defined earlier in the
   translation unit). */
static QPType *qp_best_base(JSContext *ctx, JSValueConst bases);
static QPType *qp_calculate_metaclass(JSContext *ctx, QPType *meta, JSValueConst bases);

/* _PyType_DocWithoutSignature: doc without a "name(...)\n--\n\n" block */
static const char *qp_capi_doc_without_signature(const char *name, const char *doc)
{
    size_t n;
    const char *end;
    if (!doc)
        return NULL;
    n = strlen(name);
    if (strncmp(doc, name, n) || doc[n] != '(')
        return doc;
    end = strstr(doc, ")\n--\n\n");
    if (!end)
        return doc;
    return end + 6;
}

/* find_best_base on the C side (checks that the bases are types) */
static PyTypeObject *qp_capi_best_base(PyObject *bases)
{
    JSContext *ctx = qp_capi.ctx;
    JSValue bj = qp_capi_value(ctx, bases);
    QPType *best;
    Py_ssize_t i;
    if (JS_IsException(bj))
        return NULL;
    for (i = 0; i < PyTuple_GET_SIZE(bases); i++) {
        PyObject *b = PyTuple_GET_ITEM(bases, i);
        if (!PyType_Check(b)) {
            PyErr_Format(PyExc_TypeError, "bases must be types; got '%T'", b);
            return NULL;
        }
        if (!(((PyTypeObject *)b)->tp_flags & Py_TPFLAGS_BASETYPE)) {
            PyErr_Format(PyExc_TypeError, "type '%.100s' is not an acceptable base type",
                         ((PyTypeObject *)b)->tp_name);
            return NULL;
        }
    }
    best = qp_best_base(ctx, bj);
    return best ? qp_capi_type_from_qp(ctx, best) : NULL;
}

/* type_from_slots_or_spec (CPython 3.15) */
static PyObject *qp_capi_type_from_slots_or_spec(PySlot *slots, PyType_Spec *spec,
                                                 PyTypeObject *metaclass, PyObject *module,
                                                 PyObject *bases_in)
{
    JSContext *ctx = qp_capi.ctx;
    PyHeapTypeObject *res = NULL;
    PyTypeObject *type, *base;
    PyObject *bases = NULL, *ht_name = NULL, *bases_slot = NULL;
    JSValue dict;
    char *tp_doc = NULL, *ht_tpname = NULL;
    Py_ssize_t nmembers = 0, basicsize = 0, extra_basicsize = 0, itemsize = 0;
    Py_ssize_t max_relative_offset = 0, type_data_offset, weaklistoffset, dictoffset, vectorcalloffset;
    const PyMemberDef *weaklistoffset_member = NULL, *dictoffset_member = NULL,
        *vectorcalloffset_member = NULL;
    bool have_relative_members = false;
    int flags = 0, r;
    void *token = NULL;
    QPCapiSlotIter it;
    QPCapiTypeInfo *ti;
    const char *s;
    JSValue v;

    if (!ctx) {
        Py_FatalError("PyType_FromSpec: the C API is not initialized");
        return NULL;
    }
    if (spec) {
        if (spec->basicsize > 0)
            basicsize = spec->basicsize;
        if (spec->basicsize < 0)
            extra_basicsize = -spec->basicsize;
        itemsize = spec->itemsize;
        flags = (int)spec->flags;
        qp_capi_slotiter_init_legacy(&it, spec->slots, QP_CAPI_SLOT_KIND_TYPE);
        it.name = spec->name;
    } else {
        qp_capi_slotiter_init(&it, slots, QP_CAPI_SLOT_KIND_TYPE);
    }

#define QP_CAPI_NO_SPEC                                                                 \
    if (spec) {                                                                         \
        PyErr_Format(PyExc_SystemError, "%s must not be used with PyType_Spec",         \
                     qp_capi_slot_get_name(it.current.sl_id));                          \
        goto finally;                                                                   \
    }

    while (qp_capi_slotiter_next(&it)) {
        switch (it.current.sl_id) {
        case Py_slot_invalid:
            goto finally;
        case Py_tp_name:
            QP_CAPI_NO_SPEC;
            it.name = it.current.sl_ptr;
            break;
        case Py_tp_metaclass:
            QP_CAPI_NO_SPEC;
            metaclass = it.current.sl_ptr;
            break;
        case Py_tp_module:
            QP_CAPI_NO_SPEC;
            module = it.current.sl_ptr;
            break;
        case Py_tp_bases:
            bases_slot = it.current.sl_ptr;
            break;
        case Py_tp_base:
            if (!qp_capi_slotiter_saw(&it, Py_tp_bases))
                bases_slot = it.current.sl_ptr;
            break;
        case Py_tp_basicsize:
            QP_CAPI_NO_SPEC;
            basicsize = it.current.sl_size;
            if (basicsize <= 0) {
                PyErr_SetString(PyExc_SystemError, "Py_tp_basicsize must be positive");
                goto finally;
            }
            break;
        case Py_tp_extra_basicsize:
            QP_CAPI_NO_SPEC;
            extra_basicsize = it.current.sl_size;
            if (extra_basicsize <= 0) {
                PyErr_SetString(PyExc_SystemError, "Py_tp_extra_basicsize must be positive");
                goto finally;
            }
            break;
        case Py_tp_itemsize:
            QP_CAPI_NO_SPEC;
            itemsize = it.current.sl_size;
            if (itemsize <= 0) {
                PyErr_SetString(PyExc_SystemError, "Py_tp_itemsize must be positive");
                goto finally;
            }
            break;
        case Py_tp_flags:
            QP_CAPI_NO_SPEC;
            flags = (int)it.current.sl_uint64;
            break;
        case Py_tp_members: {
            const PyMemberDef *memb;
            for (memb = it.current.sl_ptr; memb->name; memb++) {
                nmembers++;
                if (memb->flags & Py_RELATIVE_OFFSET) {
                    if (memb->offset < 0) {
                        PyErr_SetString(PyExc_SystemError, "Member offset must not be negative");
                        goto finally;
                    }
                    have_relative_members = true;
                    max_relative_offset = Py_MAX(max_relative_offset, memb->offset);
                }
                if (!strcmp(memb->name, "__weaklistoffset__"))
                    weaklistoffset_member = memb;
                else if (!strcmp(memb->name, "__dictoffset__"))
                    dictoffset_member = memb;
                else if (!strcmp(memb->name, "__vectorcalloffset__"))
                    vectorcalloffset_member = memb;
            }
            break;
        }
        case Py_tp_token:
            token = it.current.sl_ptr;
            if (token == Py_TP_USE_SPEC) {
                if (!spec) {
                    PyErr_SetString(PyExc_SystemError,
                                    "Py_tp_token: Py_TP_USE_SPEC (NULL) can only be used with PyType_Spec");
                    goto finally;
                }
                token = spec;
            }
            break;
        case Py_tp_doc:
            qp_sys_free(tp_doc);
            tp_doc = NULL;
            if (it.current.sl_ptr) {
                tp_doc = qp_capi_strdup(it.current.sl_ptr);
                if (!tp_doc)
                    goto finally;
            }
            break;
        default:
            break;
        }
    }
#undef QP_CAPI_NO_SPEC

    if (!it.name) {
        PyErr_SetString(PyExc_SystemError, spec ? "Type spec does not define the name field."
                                                : "Py_tp_name slot is required.");
        goto finally;
    }
    if (qp_capi_slotiter_saw(&it, Py_tp_basicsize) && qp_capi_slotiter_saw(&it, Py_tp_extra_basicsize)) {
        PyErr_Format(PyExc_SystemError,
                     "type %s: Py_tp_basicsize and Py_tp_extra_basicsize are mutually exclusive",
                     it.name);
        goto finally;
    }
    if (have_relative_members) {
        if (!extra_basicsize) {
            PyErr_SetString(PyExc_SystemError, "With Py_RELATIVE_OFFSET, basicsize must be extended");
            goto finally;
        }
        if (max_relative_offset >= extra_basicsize) {
            PyErr_SetString(PyExc_SystemError, "Member offset out of range (0..extra_basicsize)");
            goto finally;
        }
    }
    s = strrchr(it.name, '.');
    ht_name = PyUnicode_FromString(s ? s + 1 : it.name);
    if (!ht_name)
        goto finally;
    ht_tpname = qp_capi_strdup(it.name);
    if (!ht_tpname)
        goto finally;
    if (!bases_in)
        bases_in = bases_slot;
    if (bases_in)
        bases = PyTuple_Check(bases_in) ? Py_NewRef(bases_in) : PyTuple_Pack(1, bases_in);
    else
        bases = PyTuple_Pack(1, (PyObject *)&PyBaseObject_Type);
    if (!bases)
        goto finally;
    if ((flags & Py_TPFLAGS_IMMUTABLETYPE) && qp_capi_check_immutable_bases(it.name, bases) < 0)
        goto finally;
    base = qp_capi_best_base(bases);
    if (!base)
        goto finally;
    if (!metaclass)
        metaclass = &PyType_Type;
    {
        QPType *mq = qp_capi_type_qp(metaclass), *calc;
        JSValue bj = qp_capi_value(ctx, bases);
        if (!mq || JS_IsException(bj))
            goto finally;
        calc = qp_calculate_metaclass(ctx, mq, bj);
        if (!calc)
            goto finally;
        metaclass = qp_capi_type_from_qp(ctx, calc);
        if (!metaclass)
            goto finally;
    }
    if (!PyType_Check((PyObject *)metaclass)) {
        PyErr_Format(PyExc_TypeError, "Metaclass '%R' is not a subclass of 'type'.", metaclass);
        goto finally;
    }
    if (metaclass->tp_new && metaclass->tp_new != PyType_Type.tp_new) {
        PyErr_SetString(PyExc_TypeError, "Metaclasses with custom tp_new are not supported.");
        goto finally;
    }
    type_data_offset = basicsize;
    if (extra_basicsize) {
        type_data_offset = QP_CAPI_ALIGN_UP(base->tp_basicsize);
        basicsize = type_data_offset + QP_CAPI_ALIGN_UP(extra_basicsize);
        if (base->tp_itemsize && !((base->tp_flags | flags) & Py_TPFLAGS_ITEMS_AT_END)) {
            PyErr_SetString(PyExc_SystemError,
                            "Cannot extend variable-size class without Py_TPFLAGS_ITEMS_AT_END.");
            goto finally;
        }
    }
    if (basicsize == 0)
        basicsize = base->tp_basicsize;
    if (qp_capi_special_offset(weaklistoffset_member, type_data_offset, &weaklistoffset) < 0 ||
        qp_capi_special_offset(dictoffset_member, type_data_offset, &dictoffset) < 0 ||
        qp_capi_special_offset(vectorcalloffset_member, type_data_offset, &vectorcalloffset) < 0)
        goto finally;

    /* allocate the PyHeapTypeObject (C-primary until it is bound below) */
    res = (PyHeapTypeObject *)metaclass->tp_alloc(metaclass, nmembers);
    if (!res)
        goto finally;
    type = &res->ht_type;
    ti = qp_capi_tinfo_new(type, QP_CAPI_TO_CHEAP);
    if (!ti)
        goto finally;
    type->tp_flags = (unsigned long)flags | Py_TPFLAGS_HEAPTYPE;
    res->ht_module = Py_XNewRef(module);
    type->tp_as_async = &res->as_async;
    type->tp_as_number = &res->as_number;
    type->tp_as_sequence = &res->as_sequence;
    type->tp_as_mapping = &res->as_mapping;
    type->tp_as_buffer = &res->as_buffer;
    type->tp_base = (PyTypeObject *)Py_NewRef((PyObject *)base);
    type->tp_bases = bases;
    bases = NULL;
    type->tp_doc = tp_doc;
    ti->doc_buf = tp_doc;
    tp_doc = NULL;
    res->ht_qualname = Py_NewRef(ht_name);
    res->ht_name = ht_name;
    ht_name = NULL;
    type->tp_name = ht_tpname;
    res->_ht_tpname = ht_tpname;
    ht_tpname = NULL;
    res->ht_token = token;
    type->tp_basicsize = basicsize;
    type->tp_itemsize = itemsize;

    /* second pass: copy the slots into the type */
    qp_capi_slotiter_rewind(&it, spec ? (const void *)spec->slots : (const void *)slots);
    while (qp_capi_slotiter_next(&it)) {
        switch (it.current.sl_id) {
        case Py_slot_invalid:
            goto finally;
        case Py_tp_base:
        case Py_tp_bases:
        case Py_tp_doc:
            break;
        case Py_tp_members: {
            PyMemberDef *dst = (PyMemberDef *)((uint8_t *)res + Py_TYPE(res)->tp_basicsize);
            Py_ssize_t i;
            memcpy(dst, it.current.sl_ptr, nmembers * sizeof(PyMemberDef));
            type->tp_members = dst;
            for (i = 0; i < nmembers; i++) {
                if (dst[i].flags & Py_RELATIVE_OFFSET) {
                    dst[i].flags &= ~Py_RELATIVE_OFFSET;
                    dst[i].offset += type_data_offset;
                }
            }
            break;
        }
        default:
            qp_capi_slot_heaptype_apply_field_slot(res, it.current);
            break;
        }
    }
    if (!type->tp_dealloc)
        type->tp_dealloc = qp_capi_subtype_dealloc;
    type->tp_vectorcall_offset = vectorcalloffset;
    type->tp_weaklistoffset = weaklistoffset;
    type->tp_dictoffset = dictoffset;
    if ((type->tp_flags & Py_TPFLAGS_HAVE_VECTORCALL) && !vectorcalloffset) {
        PyErr_Format(PyExc_SystemError, "type %s: Py_TPFLAGS_HAVE_VECTORCALL requires __vectorcalloffset__",
                     type->tp_name);
        goto finally;
    }
    if (qp_capi_type_ready_preheader(type, type->tp_base) < 0)
        goto finally;

    /* bind: the runtime type object is created by the TYPE kind to_js */
    v = qp_capi_value(ctx, (PyObject *)res);
    if (JS_IsException(v))
        goto finally;
    if (qp_capi_c_type_finish(ctx, ti, 1) < 0)
        goto finally;
    /* __doc__ without the signature, __module__ */
    dict = ti->qt->dict;
    if (type->tp_doc) {
        JSValue d = qp_str_from_cstr(ctx, qp_capi_doc_without_signature(s ? s + 1 : it.name,
                                                                          type->tp_doc));
        if (JS_IsException(d) || qp_dict_set_item_atom(ctx, dict, QP_ATOM(ctx, __doc__), d) < 0) {
            JS_FreeValue(ctx, d);
            goto finally;
        }
        JS_FreeValue(ctx, d);
    }
    {
        JSValue tmp;
        r = qp_dict_get_item_atom(ctx, dict, QP_ATOM(ctx, __module__), &tmp);
        if (r > 0)
            JS_FreeValue(ctx, tmp);
    }
    if (r < 0)
        goto finally;
    if (r == 0) {
        s = strrchr(it.name, '.');
        if (s) {
            JSValue mn = qp_str_from_utf8(ctx, it.name, (size_t)(s - it.name));
            if (JS_IsException(mn))
                goto finally;
            r = qp_dict_set_item_atom(ctx, dict, QP_ATOM(ctx, __module__), mn);
            JS_FreeValue(ctx, mn);
            if (r < 0)
                goto finally;
        } else if (PyErr_WarnFormat(PyExc_DeprecationWarning, 1,
                                    "builtin type %.200s has no __module__ attribute", it.name)) {
            goto finally;
        }
    }
    qp_type_modified(ctx, ti->qt);

finally:
    if (JS_HasException(ctx))
        Py_CLEAR(res);
    Py_XDECREF(bases);
    qp_sys_free(tp_doc);
    Py_XDECREF(ht_name);
    qp_sys_free(ht_tpname);
    return (PyObject *)res;
}

PyObject *PyType_FromSlots(PySlot *slots)
{
    return qp_capi_type_from_slots_or_spec(slots, NULL, NULL, NULL, NULL);
}

PyObject *PyType_FromMetaclass(PyTypeObject *metaclass, PyObject *module, PyType_Spec *spec,
                               PyObject *bases)
{
    return qp_capi_type_from_slots_or_spec(NULL, spec, metaclass, module, bases);
}

PyObject *PyType_FromModuleAndSpec(PyObject *module, PyType_Spec *spec, PyObject *bases)
{
    return qp_capi_type_from_slots_or_spec(NULL, spec, NULL, module, bases);
}

PyObject *PyType_FromSpecWithBases(PyType_Spec *spec, PyObject *bases)
{
    return qp_capi_type_from_slots_or_spec(NULL, spec, NULL, NULL, bases);
}

PyObject *PyType_FromSpec(PyType_Spec *spec)
{
    return qp_capi_type_from_slots_or_spec(NULL, spec, NULL, NULL, NULL);
}

/* ========================================================================= */
/* Allocation                                                                 */
/* ========================================================================= */

/* The static builtin type giving the instance layout of a C type
   (PyBaseObject_Type for natives over object). */
static PyTypeObject *qp_capi_builtin_solid(PyTypeObject *type)
{
    PyTypeObject *t;
    QPCapiTypeInfo *tinfo = qp_capi_tinfo(type);
    QPType *q = tinfo ? tinfo->qt : NULL;
    if (q) {
        /* the runtime base chain follows the best base: numpy.float64 has
           tp_base numpy.floating but the layout of its other base, float */
        for (; q; q = q->base) {
            QPCapiTypeInfo *ti;
            t = qp_capi_type_by_qp_any(q);
            ti = t ? qp_capi_tinfo(t) : NULL;
            if (ti && ti->origin == QP_CAPI_TO_BUILTIN)
                return t;
        }
        return &PyBaseObject_Type;
    }
    for (t = type; t; t = t->tp_base) {
        QPCapiTypeInfo *ti = qp_capi_tinfo(t);
        if (ti && ti->origin == QP_CAPI_TO_BUILTIN)
            return t;
    }
    return &PyBaseObject_Type;
}

/* Kind of a C-primary instance of `type` made from C (PyType_GenericAlloc,
   PyObject_Init of a PyObject_Malloc block): FLOAT or COMPLEX when the
   type's builtin solid base is float or complex (numpy.float64,
   numpy.complex128: the C code fills ob_fval / cval and the JS object is
   made from them at the first crossing), else NATIVE. */
static QPCapiKind qp_capi_c_primary_kind(PyTypeObject *type)
{
    PyTypeObject *solid = qp_capi_builtin_solid(type);
    if (solid == &PyFloat_Type)
        return QP_CAPI_K_FLOAT;
    if (solid == &PyComplex_Type)
        return QP_CAPI_K_COMPLEX;
    return QP_CAPI_K_NATIVE;
}

/* zeroed C-primary float / complex instance of a subtype (see above) */
static PyObject *qp_capi_value_alloc_type(PyTypeObject *type, PyTypeObject *solid)
{
    QPCapiKind kind = solid == &PyFloat_Type ? QP_CAPI_K_FLOAT : QP_CAPI_K_COMPLEX;
    size_t size = qp_capi_kind_default_size[kind];
    PyObject *op;

    if ((size_t)type->tp_basicsize > size)
        size = (size_t)type->tp_basicsize;
    op = qp_capi_alloc(type, size, kind);
    if (op && (size_t)type->tp_basicsize > qp_capi_kind_default_size[kind])
        QP_CAPI_HEAD(op)->flags |= QP_CAPI_F_NATIVE | QP_CAPI_F_PERSIST;
    return op;
}

PyObject *_PyType_AllocNoTrack(PyTypeObject *type, Py_ssize_t nitems)
{
    JSContext *ctx = qp_capi.ctx;
    PyTypeObject *solid;
    PyObject *op;
    size_t size;
    QPCapiKind kind;

    if (!ctx) {
        Py_FatalError("PyType_GenericAlloc: the C API is not initialized");
        return NULL;
    }
    if (nitems < 0 || (type->tp_itemsize && nitems > (PY_SSIZE_T_MAX - type->tp_basicsize) / type->tp_itemsize - 1))
        return PyErr_NoMemory();
    if (PyType_IsSubtype(type, &PyType_Type)) {
        kind = QP_CAPI_K_TYPE;
    } else {
        solid = qp_capi_builtin_solid(type);
        if (solid == &PyTuple_Type)
            return qp_capi_tuple_alloc_type(type, nitems);
        if (solid == &PyFloat_Type || solid == &PyComplex_Type)
            return qp_capi_value_alloc_type(type, solid);
        if (solid == &PyBytes_Type) {
            /* bytes are J-backed (capi_bytes.c): nitems zero bytes that C
               fills in place through PyBytes_AS_STRING before sharing the
               object (numpy's PyArray_Scalar for numpy.bytes_) */
            QPType *sub = qp_capi_type_qp(type);
            if (!sub)
                return NULL;
            return qp_capi_from_js_result(ctx, qp_bytes_new_type(ctx, sub, NULL, nitems));
        }
        if (solid != &PyBaseObject_Type) {
            /* layout of a builtin (dict, list, BaseException, ...): the
               instance is made by the builtin's __new__ with the subtype */
            QPType *b = qp_capi_type_qp(solid), *sub = qp_capi_type_qp(type);
            if (!b || !sub)
                return NULL;
            if (!b->slots.tp_new) {
                PyErr_Format(PyExc_TypeError, "cannot create '%s' instances", type->tp_name);
                return NULL;
            }
            return qp_capi_from_js_result(ctx, b->slots.tp_new(ctx, sub, NULL, 0, QP_NULL));
        }
        kind = QP_CAPI_K_NATIVE;
    }
    size = (size_t)type->tp_basicsize + (size_t)(nitems + 1) * (size_t)type->tp_itemsize;
    size = (size + sizeof(void *) - 1) & ~(sizeof(void *) - 1);
    op = qp_capi_alloc(type, size, kind);
    if (!op)
        return NULL;
    if (kind == QP_CAPI_K_TYPE)
        QP_CAPI_HEAD(op)->flags |= QP_CAPI_F_NATIVE;
    if (type->tp_itemsize)
        Py_SET_SIZE(op, nitems);
    return op;
}

PyObject *PyType_GenericAlloc(PyTypeObject *type, Py_ssize_t nitems)
{
    PyObject *op = _PyType_AllocNoTrack(type, nitems);
    if (op && (type->tp_flags & Py_TPFLAGS_HAVE_GC) && QP_CAPI_HAS_HEAD(op) &&
        !(QP_CAPI_HEAD(op)->flags & QP_CAPI_F_TRACKED) &&
        (QP_CAPI_HEAD(op)->flags & QP_CAPI_F_NATIVE))
        PyObject_GC_Track(op);
    return op;
}

PyObject *PyType_GenericNew(PyTypeObject *type, PyObject *args, PyObject *kwds)
{
    return type->tp_alloc(type, 0);
}

/* ========================================================================= */
/* PyType_* API                                                              */
/* ========================================================================= */

int PyType_IsSubtype(PyTypeObject *a, PyTypeObject *b)
{
    QPCapiTypeInfo *ta, *tb;
    PyObject *mro;
    if (a == b)
        return 1;
    ta = qp_capi_tinfo(a);
    tb = qp_capi_tinfo(b);
    if (ta && tb && ta->qt && tb->qt)
        return qp_type_is_subtype(ta->qt, tb->qt);
    if (!tb && b != &PyBaseObject_Type)
        return 0;       /* unbound types (PyFrameLocalsProxy_Type) have no instances */
    mro = a->tp_mro;
    if (mro && PyTuple_Check(mro)) {
        Py_ssize_t i;
        for (i = 0; i < PyTuple_GET_SIZE(mro); i++)
            if (PyTuple_GET_ITEM(mro, i) == (PyObject *)b)
                return 1;
        return 0;
    }
    for (a = a->tp_base; a; a = a->tp_base)
        if (a == b)
            return 1;
    return b == &PyBaseObject_Type;
}

void *PyType_GetSlot(PyTypeObject *type, int slot_in)
{
    uint16_t slot = qp_capi_slot_resolve_type_slot((uint16_t)slot_in);
    return qp_capi_slot_type_getslot(type, slot);
}

unsigned long PyType_GetFlags(PyTypeObject *type)
{
    return type->tp_flags;
}

const char *_PyType_Name(PyTypeObject *type)
{
    const char *s = strrchr(type->tp_name, '.');
    return s ? s + 1 : type->tp_name;
}

static PyObject *qp_capi_type_attr(PyTypeObject *type, JSAtom atom)
{
    JSContext *ctx = qp_capi.ctx;
    JSValue v = qp_capi_value(ctx, (PyObject *)type);
    if (JS_IsException(v))
        return NULL;
    return qp_capi_from_js_result(ctx, qp_getattr(ctx, v, atom));
}

PyObject *PyType_GetName(PyTypeObject *type)
{
    return qp_capi_type_attr(type, QP_ATOM(qp_capi.ctx, __name__));
}

PyObject *PyType_GetQualName(PyTypeObject *type)
{
    return qp_capi_type_attr(type, QP_ATOM(qp_capi.ctx, __qualname__));
}

PyObject *PyType_GetModuleName(PyTypeObject *type)
{
    return qp_capi_type_attr(type, QP_ATOM(qp_capi.ctx, __module__));
}

PyObject *PyType_GetFullyQualifiedName(PyTypeObject *type)
{
    PyObject *qualname, *module, *res;
    if (!(type->tp_flags & Py_TPFLAGS_HEAPTYPE))
        return PyUnicode_FromString(type->tp_name);
    qualname = PyType_GetQualName(type);
    if (!qualname)
        return NULL;
    module = PyType_GetModuleName(type);
    if (!module) {
        Py_DECREF(qualname);
        return NULL;
    }
    if (PyUnicode_Check(module) && PyUnicode_CompareWithASCIIString(module, "builtins") != 0 &&
        PyUnicode_CompareWithASCIIString(module, "__main__") != 0)
        res = PyUnicode_FromFormat("%U.%U", module, qualname);
    else
        res = Py_NewRef(qualname);
    Py_DECREF(module);
    Py_DECREF(qualname);
    return res;
}

PyObject *PyType_GetDict(PyTypeObject *type)
{
    QPType *t = qp_capi_type_qp(type);
    if (!t)
        return NULL;
    return qp_capi_new_ref(qp_capi.ctx, t->dict);
}

PyObject *_PyType_GetDict(PyTypeObject *type)
{
    QPType *t = qp_capi_type_qp(type);
    if (!t)
        return NULL;
    if (qp_capi_type_sync_tuples(qp_capi.ctx, type, t) < 0)
        return NULL;
    return type->tp_dict;
}

void PyType_Modified(PyTypeObject *type)
{
    QPCapiTypeInfo *ti = qp_capi_tinfo(type);
    if (ti && ti->qt && qp_capi.ctx)
        qp_type_modified(qp_capi.ctx, ti->qt);
}

unsigned int PyType_ClearCache(void)
{
    JSContext *ctx = qp_capi.ctx;
    QPState *s;
    size_t i;
    if (!ctx)
        return 0;
    s = QP_STATE(ctx);
    for (i = 0; i < countof(s->mcache); i++) {
        s->mcache[i].version = 0;
        s->mcache[i].name = JS_ATOM_NULL;
        s->mcache[i].value = QP_NULL;
    }
    return s->next_type_version ? s->next_type_version - 1 : 0;
}

int PyUnstable_Type_AssignVersionTag(PyTypeObject *type)
{
    QPType *t = qp_capi_type_qp(type);
    if (!t) {
        PyErr_Clear();
        return 0;
    }
    type->tp_version_tag = qp_type_version(qp_capi.ctx, t);
    return type->tp_version_tag != 0;
}

int PyType_Freeze(PyTypeObject *type)
{
    QPType *t = qp_capi_type_qp(type);
    PyObject *mro;
    Py_ssize_t i;
    if (!t)
        return -1;
    if (qp_capi_type_sync_tuples(qp_capi.ctx, type, t) < 0)
        return -1;
    mro = type->tp_mro;
    if (!mro || !PyTuple_Check(mro)) {
        PyErr_SetString(PyExc_TypeError, "unable to get the type MRO");
        return -1;
    }
    for (i = 1; i < PyTuple_GET_SIZE(mro); i++) {
        PyTypeObject *b = (PyTypeObject *)PyTuple_GET_ITEM(mro, i);
        if (!PyType_HasFeature(b, Py_TPFLAGS_IMMUTABLETYPE)) {
            PyErr_Format(PyExc_TypeError, "Creating immutable type %s from mutable base %N",
                         type->tp_name, b);
            return -1;
        }
    }
    t->flags |= QP_TPFLAGS_IMMUTABLETYPE;
    type->tp_flags |= Py_TPFLAGS_IMMUTABLETYPE;
    qp_type_modified(qp_capi.ctx, t);
    return 0;
}

/* ---- lookups ------------------------------------------------------------- */

PyObject *_PyType_LookupRef(PyTypeObject *type, PyObject *name)
{
    JSContext *ctx = qp_capi.ctx;
    QPType *t = qp_capi_type_qp(type);
    JSAtom a;
    JSValue v;
    PyObject *r;
    JSValue saved = qp_capi_exc_save(ctx);

    if (!t) {
        qp_capi_exc_restore(ctx, saved);
        return NULL;
    }
    a = qp_capi_atom_of(ctx, name);
    if (a == JS_ATOM_NULL) {
        if (JS_HasException(ctx))
            JS_FreeValue(ctx, JS_GetException(ctx));
        qp_capi_exc_restore(ctx, saved);
        return NULL;
    }
    v = qp_type_lookup(ctx, t, a);
    JS_FreeAtom(ctx, a);
    r = QP_IS_NULL(v) ? NULL : qp_capi_new_ref(ctx, v);
    if (!r && JS_HasException(ctx))
        JS_FreeValue(ctx, JS_GetException(ctx));
    qp_capi_exc_restore(ctx, saved);
    return r;
}

/* Pin holder of a head-less (static) type: a private object whose pins
   keep borrowed results alive. */
static PyObject *qp_capi_type_holder(PyTypeObject *type)
{
    QPCapiTypeInfo *ti;
    if (QP_CAPI_HAS_HEAD(type))
        return (PyObject *)type;
    ti = qp_capi_tinfo(type);
    if (!ti)
        return NULL;
    if (!ti->holder)
        ti->holder = qp_capi_alloc(&PyBaseObject_Type, sizeof(PyObject), QP_CAPI_K_NATIVE);
    return ti->holder;
}

PyObject *_PyType_Lookup(PyTypeObject *type, PyObject *name)
{
    JSContext *ctx = qp_capi.ctx;
    QPType *t = qp_capi_type_qp(type);
    PyObject *owner, *r;
    JSAtom a;
    JSValue v;
    JSValue saved = qp_capi_exc_save(ctx);

    if (!t || !(owner = qp_capi_type_holder(type))) {
        if (JS_HasException(ctx))
            JS_FreeValue(ctx, JS_GetException(ctx));
        qp_capi_exc_restore(ctx, saved);
        return NULL;
    }
    a = qp_capi_atom_of(ctx, name);
    if (a == JS_ATOM_NULL) {
        if (JS_HasException(ctx))
            JS_FreeValue(ctx, JS_GetException(ctx));
        qp_capi_exc_restore(ctx, saved);
        return NULL;
    }
    v = qp_type_lookup(ctx, t, a);
    r = QP_IS_NULL(v) ? NULL : qp_capi_borrow(ctx, owner, QP_CAPI_PIN_HASHED(a), v);
    JS_FreeAtom(ctx, a);
    if (!r && JS_HasException(ctx))
        JS_FreeValue(ctx, JS_GetException(ctx));
    qp_capi_exc_restore(ctx, saved);
    return r;
}

/* _PyObject_GetState: object.__getstate__() */
PyObject *_PyObject_GetState(PyObject *obj)
{
    JSContext *ctx = qp_capi.ctx;
    JSValue v = qp_capi_value(ctx, obj);
    if (JS_IsException(v))
        return NULL;
    return qp_capi_from_js_result(ctx, qp_call_method(ctx, v, QP_ATOM(ctx, __getstate__), NULL, 0, QP_NULL));
}

/* ---- modules of heap types ------------------------------------------------ */

static void *qp_capi_module_token(PyObject *module)
{
    if (!module || !PyModule_Check(module))
        return NULL;
    return ((QPCapiModuleObject *)module)->md_token;
}

PyObject *PyType_GetModule_DuringGC(PyTypeObject *type)
{
    if (!(type->tp_flags & Py_TPFLAGS_HEAPTYPE))
        return NULL;
    return ((PyHeapTypeObject *)type)->ht_module;
}

PyObject *PyType_GetModule(PyTypeObject *type)
{
    PyHeapTypeObject *et;
    if (!(type->tp_flags & Py_TPFLAGS_HEAPTYPE)) {
        PyErr_Format(PyExc_TypeError, "PyType_GetModule: Type '%s' is not a heap type", type->tp_name);
        return NULL;
    }
    et = (PyHeapTypeObject *)type;
    if (!et->ht_module) {
        PyErr_Format(PyExc_TypeError, "PyType_GetModule: Type '%s' has no associated module",
                     type->tp_name);
        return NULL;
    }
    return et->ht_module;
}

void *PyType_GetModuleState_DuringGC(PyTypeObject *type)
{
    PyObject *m = PyType_GetModule_DuringGC(type);
    return m ? ((QPCapiModuleObject *)m)->md_state : NULL;
}

void *PyType_GetModuleState(PyTypeObject *type)
{
    PyObject *m = PyType_GetModule(type);
    return m ? ((QPCapiModuleObject *)m)->md_state : NULL;
}

PyObject *PyType_GetModuleByToken_DuringGC(PyTypeObject *type, const void *token)
{
    PyObject *mro, *module;
    Py_ssize_t i;
    if (!(type->tp_flags & Py_TPFLAGS_HEAPTYPE))
        return NULL;
    module = ((PyHeapTypeObject *)type)->ht_module;
    if (module && qp_capi_module_token(module) == token)
        return module;
    mro = type->tp_mro;
    if (!mro)
        return NULL;
    for (i = 1; i < PyTuple_GET_SIZE(mro); i++) {
        PyTypeObject *super = (PyTypeObject *)PyTuple_GET_ITEM(mro, i);
        if (!(super->tp_flags & Py_TPFLAGS_HEAPTYPE))
            continue;
        module = ((PyHeapTypeObject *)super)->ht_module;
        if (module && qp_capi_module_token(module) == token)
            return module;
    }
    return NULL;
}

PyObject *PyType_GetModuleByDef(PyTypeObject *type, PyModuleDef *def)
{
    PyObject *mod = PyType_GetModuleByToken_DuringGC(type, def);
    if (!mod) {
        PyErr_Format(PyExc_TypeError,
                     "PyType_GetModuleByDef: No superclass of '%s' has the given module", type->tp_name);
        return NULL;
    }
    return mod;
}

PyObject *PyType_GetModuleByToken(PyTypeObject *type, const void *token)
{
    return Py_XNewRef(PyType_GetModuleByDef(type, (PyModuleDef *)token));
}

static PyTypeObject *qp_capi_base_by_token_rec(PyObject *bases, void *token)
{
    Py_ssize_t i;
    if (!bases)
        return NULL;
    for (i = 0; i < PyTuple_GET_SIZE(bases); i++) {
        PyTypeObject *base = (PyTypeObject *)PyTuple_GET_ITEM(bases, i), *r;
        if (!(base->tp_flags & Py_TPFLAGS_HEAPTYPE))
            continue;
        if (((PyHeapTypeObject *)base)->ht_token == token)
            return base;
        r = qp_capi_base_by_token_rec(base->tp_bases, token);
        if (r)
            return r;
    }
    return NULL;
}

int PyType_GetBaseByToken_DuringGC(PyTypeObject *type, void *token, PyTypeObject **result)
{
    PyObject *mro;
    Py_ssize_t i;
    if (result)
        *result = NULL;
    if (!token || !PyType_Check((PyObject *)type))
        return -1;
    if (!(type->tp_flags & Py_TPFLAGS_HEAPTYPE))
        return 0;
    if (((PyHeapTypeObject *)type)->ht_token == token)
        goto found;
    mro = type->tp_mro;
    if (!mro) {
        PyTypeObject *base = qp_capi_base_by_token_rec(type->tp_bases, token);
        if (!base)
            return 0;
        type = base;
        goto found;
    }
    for (i = 1; i < PyTuple_GET_SIZE(mro); i++) {
        PyTypeObject *base = (PyTypeObject *)PyTuple_GET_ITEM(mro, i);
        if ((base->tp_flags & Py_TPFLAGS_HEAPTYPE) && ((PyHeapTypeObject *)base)->ht_token == token) {
            type = base;
            goto found;
        }
    }
    return 0;
found:
    if (result)
        *result = type;
    return 1;
}

int PyType_GetBaseByToken(PyTypeObject *type, void *token, PyTypeObject **result)
{
    int res;
    if (result)
        *result = NULL;
    if (!token) {
        PyErr_Format(PyExc_SystemError, "PyType_GetBaseByToken called with token=NULL");
        return -1;
    }
    if (!PyType_Check((PyObject *)type)) {
        PyErr_Format(PyExc_TypeError, "expected a type, got a '%T' object", type);
        return -1;
    }
    res = PyType_GetBaseByToken_DuringGC(type, token, result);
    if (res > 0 && result)
        Py_INCREF(*result);
    return res;
}

/* ---- PEP 697 type data ---------------------------------------------------- */

void *PyObject_GetTypeData_DuringGC(PyObject *obj, PyTypeObject *cls)
{
    return (char *)obj + QP_CAPI_ALIGN_UP(cls->tp_base->tp_basicsize);
}

void *PyObject_GetTypeData(PyObject *obj, PyTypeObject *cls)
{
    return PyObject_GetTypeData_DuringGC(obj, cls);
}

Py_ssize_t PyType_GetTypeDataSize(PyTypeObject *cls)
{
    ptrdiff_t r = cls->tp_basicsize - QP_CAPI_ALIGN_UP(cls->tp_base->tp_basicsize);
    return r < 0 ? 0 : r;
}

static void *qp_capi_item_data(PyObject *obj, int raise)
{
    if (!(Py_TYPE(obj)->tp_flags & Py_TPFLAGS_ITEMS_AT_END)) {
        if (raise)
            PyErr_Format(PyExc_TypeError, "type '%T' does not have Py_TPFLAGS_ITEMS_AT_END", obj);
        return NULL;
    }
    return (char *)obj + Py_TYPE(obj)->tp_basicsize;
}

void *PyObject_GetItemData_DuringGC(PyObject *obj)
{
    return qp_capi_item_data(obj, 0);
}

void *PyObject_GetItemData(PyObject *obj)
{
    return qp_capi_item_data(obj, 1);
}

/* ---- type watchers --------------------------------------------------------- */

#define QP_CAPI_TYPE_MAX_WATCHERS 8
static PyType_WatchCallback qp_capi_type_watchers[QP_CAPI_TYPE_MAX_WATCHERS];

static PyTypeObject *qp_capi_type_watched(PyTypeObject *tp)
{
    int i;
    unsigned char bits = tp->tp_watched;
    for (i = 0; bits && i < QP_CAPI_TYPE_MAX_WATCHERS; i++, bits >>= 1) {
        if ((bits & 1) && qp_capi_type_watchers[i] && qp_capi_type_watchers[i](tp) < 0)
            PyErr_FormatUnraisable("Exception ignored in type watcher callback #%d for %R", i, tp);
    }
    return tp;
}

static int qp_capi_validate_type_watcher(int id)
{
    if (id < 0 || id >= QP_CAPI_TYPE_MAX_WATCHERS) {
        PyErr_Format(PyExc_ValueError, "Invalid type watcher ID %d", id);
        return -1;
    }
    if (!qp_capi_type_watchers[id]) {
        PyErr_Format(PyExc_ValueError, "No type watcher set for ID %d", id);
        return -1;
    }
    return 0;
}

int PyType_AddWatcher(PyType_WatchCallback callback)
{
    int i;
    /* Start at 1, 0 is reserved for cpython optimizer */
    for (i = 1; i < QP_CAPI_TYPE_MAX_WATCHERS; i++) {
        if (!qp_capi_type_watchers[i]) {
            qp_capi_type_watchers[i] = callback;
            qp_capi_type_watchers_set |= (uint8_t)(1 << i);
            return i;
        }
    }
    PyErr_SetString(PyExc_RuntimeError, "no more type watcher IDs available");
    return -1;
}

int PyType_ClearWatcher(int watcher_id)
{
    if (qp_capi_validate_type_watcher(watcher_id) < 0)
        return -1;
    qp_capi_type_watchers[watcher_id] = NULL;
    qp_capi_type_watchers_set &= (uint8_t)~(1 << watcher_id);
    return 0;
}

/* type_dealloc: the watchers of a dying watched type (qp_del_context
   defers its free to the finalizer queue, where the type is intact). */
static BOOL qp_capi_type_dealloc_watched(JSObject *p)
{
    PyObject *op;
    if (!qp_capi_type_watchers_set || !p->py_has_mirror)
        return FALSE;
    op = qp_capi_lookup(JS_MKPTR(JS_TAG_OBJECT, p));
    return op && (((PyTypeObject *)op)->tp_watched & qp_capi_type_watchers_set) != 0;
}

static void qp_capi_type_dealloc_notify(JSContext *ctx, JSValueConst obj)
{
    PyObject *op = qp_capi_new_ref(ctx, obj);
    if (!op) {
        PyErr_WriteUnraisable(NULL);
        return;
    }
    if (((PyTypeObject *)op)->tp_watched)
        qp_capi_type_watched((PyTypeObject *)op);
    Py_DECREF(op);
}

int PyType_Watch(int watcher_id, PyObject *obj)
{
    if (!PyType_Check(obj)) {
        PyErr_SetString(PyExc_ValueError, "Cannot watch non-type");
        return -1;
    }
    if (qp_capi_validate_type_watcher(watcher_id) < 0)
        return -1;
    PyUnstable_Type_AssignVersionTag((PyTypeObject *)obj);
    ((PyTypeObject *)obj)->tp_watched |= (unsigned char)(1 << watcher_id);
    return 0;
}

int PyType_Unwatch(int watcher_id, PyObject *obj)
{
    if (!PyType_Check(obj)) {
        PyErr_SetString(PyExc_ValueError, "Cannot watch non-type");
        return -1;
    }
    if (qp_capi_validate_type_watcher(watcher_id) < 0)
        return -1;
    ((PyTypeObject *)obj)->tp_watched &= (unsigned char)~(1 << watcher_id);
    return 0;
}

/* ========================================================================= */
/* PyMember_GetOne / PyMember_SetOne (CPython Python/structmember.c)          */
/* ========================================================================= */

static void *qp_capi_member_addr(PyObject *obj, PyMemberDef *mp)
{
    unsigned char *addr = (unsigned char *)obj + mp->offset;
    if (mp->flags & _Py_AFTER_ITEMS) {
        PyTypeObject *type = Py_TYPE(obj);
        size_t n = (size_t)(Py_SIZE(obj) * type->tp_itemsize);
        addr += (n + sizeof(void *) - 1) & ~(sizeof(void *) - 1);
    }
    return addr;
}

/* Py_T_BYTE is a plain `char` of the platform C ABI (the extension's
   compiler); this file is built with -funsigned-char (Makefile), so use the
   ABI's signedness explicitly (as python/mod_locale.c QP_LOCALE_CHAR_MAX). */
#if ((defined(__aarch64__) || defined(__arm__)) && !defined(__APPLE__) && !defined(_WIN32)) || \
    defined(__powerpc__) || defined(__powerpc64__) || defined(__s390__) || defined(__riscv)
typedef unsigned char qp_capi_abi_char;
#define QP_CAPI_ABI_CHAR_MIN 0
#define QP_CAPI_ABI_CHAR_MAX UCHAR_MAX
#else
typedef signed char qp_capi_abi_char;
#define QP_CAPI_ABI_CHAR_MIN SCHAR_MIN
#define QP_CAPI_ABI_CHAR_MAX SCHAR_MAX
#endif

PyObject *PyMember_GetOne(const char *obj_addr, PyMemberDef *l)
{
    PyObject *v;
    const void *addr;

    if (l->flags & Py_RELATIVE_OFFSET) {
        PyErr_SetString(PyExc_SystemError, "PyMember_GetOne used with Py_RELATIVE_OFFSET");
        return NULL;
    }
    addr = qp_capi_member_addr((PyObject *)obj_addr, l);
    switch (l->type) {
    case Py_T_BOOL: v = PyBool_FromLong(*(char *)addr); break;
    case Py_T_BYTE: v = PyLong_FromLong(*(qp_capi_abi_char *)addr); break;
    case Py_T_UBYTE: v = PyLong_FromUnsignedLong(*(unsigned char *)addr); break;
    case Py_T_SHORT: v = PyLong_FromLong(*(short *)addr); break;
    case Py_T_USHORT: v = PyLong_FromUnsignedLong(*(unsigned short *)addr); break;
    case Py_T_INT: v = PyLong_FromLong(*(int *)addr); break;
    case Py_T_UINT: v = PyLong_FromUnsignedLong(*(unsigned int *)addr); break;
    case Py_T_LONG: v = PyLong_FromLong(*(long *)addr); break;
    case Py_T_ULONG: v = PyLong_FromUnsignedLong(*(unsigned long *)addr); break;
    case Py_T_PYSSIZET: v = PyLong_FromSsize_t(*(Py_ssize_t *)addr); break;
    case Py_T_FLOAT: v = PyFloat_FromDouble((double)*(float *)addr); break;
    case Py_T_DOUBLE: v = PyFloat_FromDouble(*(double *)addr); break;
    case Py_T_STRING:
        v = *(char **)addr ? PyUnicode_FromString(*(char **)addr) : Py_NewRef(Py_None);
        break;
    case Py_T_STRING_INPLACE: v = PyUnicode_FromString((char *)addr); break;
    case Py_T_CHAR: v = PyUnicode_FromStringAndSize((char *)addr, 1); break;
    case _Py_T_OBJECT:
        v = *(PyObject **)addr;
        v = Py_NewRef(v ? v : Py_None);
        break;
    case Py_T_OBJECT_EX:
        v = *(PyObject **)addr;
        if (!v)
            PyErr_Format(PyExc_AttributeError, "'%T' object has no attribute '%s'",
                         (PyObject *)obj_addr, l->name);
        Py_XINCREF(v);
        break;
    case Py_T_LONGLONG: v = PyLong_FromLongLong(*(long long *)addr); break;
    case Py_T_ULONGLONG: v = PyLong_FromUnsignedLongLong(*(unsigned long long *)addr); break;
    case _Py_T_NONE: v = Py_NewRef(Py_None); break;
    default:
        PyErr_SetString(PyExc_SystemError, "bad memberdescr type");
        v = NULL;
    }
    return v;
}

#define QP_CAPI_WARN(msg) do { if (PyErr_WarnEx(PyExc_RuntimeWarning, msg, 1) < 0) return -1; } while (0)

/* unsigned conversions accepting negative values with a warning */
static int qp_capi_member_unsigned(PyObject *v, unsigned long long *out, int *neg)
{
    PyObject *i = PyNumber_Index(v);
    int sign;
    if (!i)
        return -1;
    if (PyLong_GetSign(i, &sign) < 0) {
        Py_DECREF(i);
        return -1;
    }
    *neg = sign < 0;
    if (*neg) {
        long long ll = PyLong_AsLongLong(i);
        Py_DECREF(i);
        if (ll == -1 && PyErr_Occurred())
            return -1;
        *out = (unsigned long long)ll;
    } else {
        unsigned long long ull = PyLong_AsUnsignedLongLong(i);
        Py_DECREF(i);
        if (ull == (unsigned long long)-1 && PyErr_Occurred())
            return -1;
        *out = ull;
    }
    return 0;
}

int PyMember_SetOne(char *addr, PyMemberDef *l, PyObject *v)
{
    PyObject *obj = (PyObject *)addr, *oldv;
    long long_val;

    if (l->flags & Py_RELATIVE_OFFSET) {
        PyErr_SetString(PyExc_SystemError, "PyMember_SetOne used with Py_RELATIVE_OFFSET");
        return -1;
    }
    addr = qp_capi_member_addr(obj, l);
    if (l->flags & Py_READONLY) {
        PyErr_SetString(PyExc_AttributeError, "readonly attribute");
        return -1;
    }
    if (!v && l->type != Py_T_OBJECT_EX && l->type != _Py_T_OBJECT) {
        PyErr_SetString(PyExc_TypeError, "can't delete numeric/char attribute");
        return -1;
    }
    switch (l->type) {
    case Py_T_BOOL:
        if (!PyBool_Check(v)) {
            PyErr_SetString(PyExc_TypeError, "attribute value type must be bool");
            return -1;
        }
        *(char *)addr = v == Py_True;
        break;
    case Py_T_BYTE:
        long_val = PyLong_AsLong(v);
        if (long_val == -1 && PyErr_Occurred())
            return -1;
        *(qp_capi_abi_char *)addr = (qp_capi_abi_char)long_val;
        if (long_val > QP_CAPI_ABI_CHAR_MAX || long_val < QP_CAPI_ABI_CHAR_MIN)
            QP_CAPI_WARN("Truncation of value to char");
        break;
    case Py_T_UBYTE:
        long_val = PyLong_AsLong(v);
        if (long_val == -1 && PyErr_Occurred())
            return -1;
        *(unsigned char *)addr = (unsigned char)long_val;
        if (long_val > UCHAR_MAX || long_val < 0)
            QP_CAPI_WARN("Truncation of value to unsigned char");
        break;
    case Py_T_SHORT:
        long_val = PyLong_AsLong(v);
        if (long_val == -1 && PyErr_Occurred())
            return -1;
        *(short *)addr = (short)long_val;
        if (long_val > SHRT_MAX || long_val < SHRT_MIN)
            QP_CAPI_WARN("Truncation of value to short");
        break;
    case Py_T_USHORT:
        long_val = PyLong_AsLong(v);
        if (long_val == -1 && PyErr_Occurred())
            return -1;
        *(unsigned short *)addr = (unsigned short)long_val;
        if (long_val > USHRT_MAX || long_val < 0)
            QP_CAPI_WARN("Truncation of value to unsigned short");
        break;
    case Py_T_INT:
        long_val = PyLong_AsLong(v);
        if (long_val == -1 && PyErr_Occurred())
            return -1;
        *(int *)addr = (int)long_val;
        if (long_val > INT_MAX || long_val < INT_MIN)
            QP_CAPI_WARN("Truncation of value to int");
        break;
    case Py_T_UINT: {
        unsigned long long u;
        int neg;
        if (qp_capi_member_unsigned(v, &u, &neg) < 0)
            return -1;
        if (!neg && u > ULONG_MAX) {
            PyErr_SetString(PyExc_OverflowError, "Python int too large to convert to C unsigned long");
            return -1;
        }
        *(unsigned int *)addr = (unsigned int)u;
        if (neg)
            QP_CAPI_WARN("Writing negative value into unsigned field");
        else if (u > UINT_MAX)
            QP_CAPI_WARN("Truncation of value to unsigned int");
        break;
    }
    case Py_T_LONG:
        long_val = PyLong_AsLong(v);
        if (long_val == -1 && PyErr_Occurred())
            return -1;
        *(long *)addr = long_val;
        break;
    case Py_T_ULONG: {
        unsigned long long u;
        int neg;
        if (qp_capi_member_unsigned(v, &u, &neg) < 0)
            return -1;
        if (!neg && u > ULONG_MAX) {
            PyErr_SetString(PyExc_OverflowError, "Python int too large to convert to C unsigned long");
            return -1;
        }
        *(unsigned long *)addr = (unsigned long)u;
        if (neg)
            QP_CAPI_WARN("Writing negative value into unsigned field");
        break;
    }
    case Py_T_PYSSIZET: {
        Py_ssize_t s = PyLong_AsSsize_t(v);
        if (s == -1 && PyErr_Occurred())
            return -1;
        *(Py_ssize_t *)addr = s;
        break;
    }
    case Py_T_FLOAT: {
        double d = PyFloat_AsDouble(v);
        if (d == -1 && PyErr_Occurred())
            return -1;
        *(float *)addr = (float)d;
        break;
    }
    case Py_T_DOUBLE: {
        double d = PyFloat_AsDouble(v);
        if (d == -1 && PyErr_Occurred())
            return -1;
        *(double *)addr = d;
        break;
    }
    case _Py_T_OBJECT:
    case Py_T_OBJECT_EX:
        oldv = *(PyObject **)addr;
        *(PyObject **)addr = Py_XNewRef(v);
        if (!v && !oldv && l->type == Py_T_OBJECT_EX) {
            PyErr_SetString(PyExc_AttributeError, l->name);
            return -1;
        }
        Py_XDECREF(oldv);
        break;
    case Py_T_CHAR: {
        Py_ssize_t len;
        const char *s = PyUnicode_AsUTF8AndSize(v, &len);
        if (!s || len != 1) {
            PyErr_BadArgument();
            return -1;
        }
        *(char *)addr = s[0];
        break;
    }
    case Py_T_STRING:
    case Py_T_STRING_INPLACE:
        PyErr_SetString(PyExc_TypeError, "readonly attribute");
        return -1;
    case Py_T_LONGLONG: {
        long long ll = PyLong_AsLongLong(v);
        if (ll == -1 && PyErr_Occurred())
            return -1;
        *(long long *)addr = ll;
        break;
    }
    case Py_T_ULONGLONG: {
        unsigned long long u;
        int neg;
        if (qp_capi_member_unsigned(v, &u, &neg) < 0)
            return -1;
        *(unsigned long long *)addr = u;
        if (neg)
            QP_CAPI_WARN("Writing negative value into unsigned field");
        break;
    }
    default:
        PyErr_Format(PyExc_SystemError, "bad memberdescr type for %s", l->name);
        return -1;
    }
    return 0;
}

#undef QP_CAPI_WARN

/* ========================================================================= */
/* Synthesized C defs of runtime builtins                                    */
/* ========================================================================= */

static QPCapiPMap qp_capi_synth_defs;   /* QPMethodDef* / QPGetSetDef* -> synthesized C def */

/* ml_meth of builtins implemented by the runtime: they are called through
   PyObject_Call* / vectorcall only (capi-design deviation). */
static PyObject *qp_capi_runtime_builtin_meth(PyObject *self, PyObject *args)
{
    PyErr_SetString(PyExc_SystemError,
                    "builtin implemented by the runtime cannot be called through its PyMethodDef; "
                    "use PyObject_Call*()");
    return NULL;
}

static PyMethodDef *qp_capi_synth_methoddef(const QPMethodDef *qml)
{
    PyMethodDef *ml = qp_capi_pmap_get(&qp_capi_synth_defs, qml, NULL);
    int f;
    if (ml)
        return ml;
    ml = qp_sys_calloc(1, sizeof(*ml));
    if (!ml) {
        PyErr_NoMemory();
        return NULL;
    }
    if (qml->flags & QP_METH_NOARGS)
        f = METH_NOARGS;
    else if (qml->flags & QP_METH_O)
        f = METH_O;
    else
        f = METH_FASTCALL | ((qml->flags & QP_METH_KEYWORDS) ? METH_KEYWORDS : 0);
    if (qml->flags & QP_METH_CLASS)
        f |= METH_CLASS;
    if (qml->flags & QP_METH_STATIC)
        f |= METH_STATIC;
    if (qml->flags & QP_METH_COEXIST)
        f |= METH_COEXIST;
    ml->ml_name = qml->name;
    ml->ml_meth = qp_capi_runtime_builtin_meth;
    ml->ml_flags = f;
    ml->ml_doc = qml->doc;
    if (qp_capi_pmap_set(&qp_capi_synth_defs, qml, NULL, ml) < 0) {
        qp_sys_free(ml);
        PyErr_NoMemory();
        return NULL;
    }
    return ml;
}

/* C def of a QP method def (the original PyMethodDef for C methods) */
static PyMethodDef *qp_capi_c_methoddef(const QPMethodDef *qml, PyTypeObject **pcls)
{
    if (pcls)
        *pcls = NULL;
    if (qml->meth == qp_capi_meth_call) {
        QPCapiDef *d = qp_capi_def_at(qml->magic);
        if (d && d->kind == QP_CAPI_DEF_METHOD) {
            if (pcls)
                *pcls = d->cls;
            return d->u.ml;
        }
    }
    return qp_capi_synth_methoddef(qml);
}

/* PyGetSetDef of runtime getsets: real C accessors over the QP getter */
static PyObject *qp_capi_synth_getset_get(PyObject *self, void *closure)
{
    JSContext *ctx = qp_capi.ctx;
    const QPGetSetDef *gs = closure;
    JSValue v = qp_capi_value(ctx, self);
    if (JS_IsException(v))
        return NULL;
    if (!gs->get) {
        PyErr_Format(PyExc_AttributeError, "attribute '%s' is not readable", gs->name);
        return NULL;
    }
    return qp_capi_from_js_result(ctx, gs->get(ctx, v, gs->magic));
}

static int qp_capi_synth_getset_set(PyObject *self, PyObject *value, void *closure)
{
    JSContext *ctx = qp_capi.ctx;
    const QPGetSetDef *gs = closure;
    JSValue v = qp_capi_value(ctx, self), vv = QP_NULL;
    if (JS_IsException(v))
        return -1;
    if (value) {
        vv = qp_capi_value(ctx, value);
        if (JS_IsException(vv))
            return -1;
    }
    if (!gs->set) {
        PyErr_Format(PyExc_AttributeError, "attribute '%s' is not writable", gs->name);
        return -1;
    }
    return gs->set(ctx, v, vv, gs->magic) < 0 ? -1 : 0;
}

static PyGetSetDef *qp_capi_c_getsetdef(const QPGetSetDef *qgs)
{
    PyGetSetDef *gs;
    if (qgs->get == qp_capi_getset_get) {
        QPCapiDef *d = qp_capi_def_at(qgs->magic);
        if (d && d->kind == QP_CAPI_DEF_GETSET)
            return d->u.gs;
    }
    gs = qp_capi_pmap_get(&qp_capi_synth_defs, qgs, NULL);
    if (gs)
        return gs;
    gs = qp_sys_calloc(1, sizeof(*gs));
    if (!gs) {
        PyErr_NoMemory();
        return NULL;
    }
    gs->name = qgs->name;
    gs->get = qp_capi_synth_getset_get;
    gs->set = qgs->set ? qp_capi_synth_getset_set : NULL;
    gs->doc = qgs->doc;
    gs->closure = (void *)qgs;
    if (qp_capi_pmap_set(&qp_capi_synth_defs, qgs, NULL, gs) < 0) {
        qp_sys_free(gs);
        PyErr_NoMemory();
        return NULL;
    }
    return gs;
}

/* vectorcall field of callable mirrors */
static PyObject *qp_capi_mirror_vectorcall(PyObject *callable, PyObject *const *args, size_t nargsf,
                                           PyObject *kwnames)
{
    return qp_capi_vectorcall(callable, args, nargsf, kwnames);
}

/* ========================================================================= */
/* Kind CFUNCTION: builtin_function_or_method                                 */
/* ========================================================================= */

static size_t qp_capi_cfunc_kind_size(JSContext *ctx, JSValueConst v)
{
    return sizeof(PyCMethodObject) - sizeof(PyObject);
}

static int qp_capi_cfunc_kind_init(JSContext *ctx, PyObject *op, JSValueConst v)
{
    PyCMethodObject *m = (PyCMethodObject *)op;
    QPCFunctionObject *f = JS_VALUE_GET_OBJ(v)->u.opaque;
    PyTypeObject *cls = NULL;

    if (!f) {
        qp_raise_system_error(ctx, "bad builtin function object");
        return -1;
    }
    m->func.m_ml = qp_capi_c_methoddef(f->ml, &cls);
    if (!m->func.m_ml)
        return -1;
    if (!QP_IS_NULL(f->self) && !(f->ml->flags & QP_METH_STATIC)) {
        m->func.m_self = qp_capi_new_ref(ctx, f->self);
        if (!m->func.m_self)
            return -1;
    }
    if (!QP_IS_NULL(f->module)) {
        m->func.m_module = qp_capi_new_ref(ctx, f->module);
        if (!m->func.m_module)
            return -1;
    }
    m->func.vectorcall = qp_capi_mirror_vectorcall;
    if (cls && (m->func.m_ml->ml_flags & METH_METHOD)) {
        PyTypeObject *old = Py_TYPE(op);
        m->mm_class = (PyTypeObject *)Py_NewRef((PyObject *)cls);
        Py_SET_TYPE(op, &PyCMethod_Type);
        if (QP_CAPI_HAS_HEAD(old))
            Py_DECREF(old);
    }
    return 0;
}

static int qp_capi_cfunc_kind_traverse(PyObject *op, visitproc visit, void *arg)
{
    PyCMethodObject *m = (PyCMethodObject *)op;
    Py_VISIT(m->func.m_self);
    Py_VISIT(m->func.m_module);
    Py_VISIT((PyObject *)m->mm_class);
    return 0;
}

static void qp_capi_cfunc_kind_clear(JSRuntime *rt, PyObject *op)
{
    PyCMethodObject *m = (PyCMethodObject *)op;
    PyObject *cls = (PyObject *)m->mm_class;
    m->mm_class = NULL;
    Py_CLEAR(m->func.m_self);
    Py_CLEAR(m->func.m_module);
    Py_XDECREF(cls);
}

PyObject *PyCMethod_New(PyMethodDef *ml, PyObject *self, PyObject *module, PyTypeObject *cls)
{
    JSContext *ctx = qp_capi.ctx;
    const QPMethodDef *qml;
    JSValue sj = QP_NULL, mj = QP_NULL;

    if (!ctx) {
        Py_FatalError("PyCFunction_New: the C API is not initialized");
        return NULL;
    }
    if (ml->ml_flags & METH_METHOD) {
        if (!cls) {
            PyErr_SetString(PyExc_SystemError,
                            "attempting to create PyCMethod with a METH_METHOD flag but no class");
            return NULL;
        }
    } else if (cls) {
        PyErr_SetString(PyExc_SystemError,
                        "attempting to create PyCFunction with class but no METH_METHOD flag");
        return NULL;
    }
    switch (ml->ml_flags & (METH_VARARGS | METH_FASTCALL | METH_NOARGS | METH_O |
                            METH_KEYWORDS | METH_METHOD)) {
    case METH_VARARGS: case METH_VARARGS | METH_KEYWORDS: case METH_NOARGS: case METH_O:
    case METH_FASTCALL: case METH_FASTCALL | METH_KEYWORDS:
    case METH_METHOD | METH_FASTCALL | METH_KEYWORDS:
        break;
    default:
        PyErr_Format(PyExc_SystemError, "%s() method: bad call flags", ml->ml_name);
        return NULL;
    }
    qml = qp_capi_method_def(ctx, ml, cls);
    if (!qml)
        return NULL;
    if (self) {
        sj = qp_capi_value(ctx, self);
        if (JS_IsException(sj))
            return NULL;
    }
    if (module) {
        mj = qp_capi_value(ctx, module);
        if (JS_IsException(mj))
            return NULL;
    }
    return qp_capi_steal(ctx, qp_cfunction_new(ctx, qml, sj, mj));
}

/* methodobject.h maps these to PyCMethod_New by macro; the functions are
   exported for the stable ABI (CPython does the same #undef). */
#undef PyCFunction_New
#undef PyCFunction_NewEx
PyObject *PyCFunction_NewEx(PyMethodDef *ml, PyObject *self, PyObject *module)
{
    return PyCMethod_New(ml, self, module, NULL);
}

PyObject *PyCFunction_New(PyMethodDef *ml, PyObject *self)
{
    return PyCMethod_New(ml, self, NULL, NULL);
}

PyCFunction PyCFunction_GetFunction(PyObject *op)
{
    if (!PyCFunction_Check(op)) {
        PyErr_BadInternalCall();
        return NULL;
    }
    return ((PyCFunctionObject *)op)->m_ml->ml_meth;
}

PyObject *PyCFunction_GetSelf(PyObject *op)
{
    PyCFunctionObject *f;
    if (!PyCFunction_Check(op)) {
        PyErr_BadInternalCall();
        return NULL;
    }
    f = (PyCFunctionObject *)op;
    return (f->m_ml->ml_flags & METH_STATIC) ? NULL : f->m_self;
}

int PyCFunction_GetFlags(PyObject *op)
{
    if (!PyCFunction_Check(op)) {
        PyErr_BadInternalCall();
        return -1;
    }
    return ((PyCFunctionObject *)op)->m_ml->ml_flags;
}

/* ========================================================================= */
/* Kind METHOD (bound methods), instancemethod                                */
/* ========================================================================= */

static size_t qp_capi_method_kind_size(JSContext *ctx, JSValueConst v)
{
    return sizeof(PyMethodObject) - sizeof(PyObject);
}

static int qp_capi_method_kind_init(JSContext *ctx, PyObject *op, JSValueConst v)
{
    PyMethodObject *m = (PyMethodObject *)op;
    QPMethodObject *q = JS_VALUE_GET_OBJ(v)->u.opaque;
    if (!q) {
        qp_raise_system_error(ctx, "bad method object");
        return -1;
    }
    m->im_func = qp_capi_new_ref(ctx, q->func);
    if (!m->im_func)
        return -1;
    m->im_self = qp_capi_new_ref(ctx, q->self);
    if (!m->im_self)
        return -1;
    m->vectorcall = qp_capi_mirror_vectorcall;
    return 0;
}

static int qp_capi_method_kind_traverse(PyObject *op, visitproc visit, void *arg)
{
    PyMethodObject *m = (PyMethodObject *)op;
    Py_VISIT(m->im_func);
    Py_VISIT(m->im_self);
    return 0;
}

static void qp_capi_method_kind_clear(JSRuntime *rt, PyObject *op)
{
    PyMethodObject *m = (PyMethodObject *)op;
    Py_CLEAR(m->im_func);
    Py_CLEAR(m->im_self);
}

PyObject *PyMethod_New(PyObject *func, PyObject *self)
{
    JSContext *ctx = qp_capi.ctx;
    JSValue fj, sj;
    if (!self) {
        PyErr_BadInternalCall();
        return NULL;
    }
    fj = qp_capi_value(ctx, func);
    if (JS_IsException(fj))
        return NULL;
    sj = qp_capi_value(ctx, self);
    if (JS_IsException(sj))
        return NULL;
    return qp_capi_steal(ctx, qp_method_new(ctx, fj, sj));
}

PyObject *PyMethod_Function(PyObject *im)
{
    if (!PyMethod_Check(im)) {
        PyErr_BadInternalCall();
        return NULL;
    }
    return ((PyMethodObject *)im)->im_func;
}

PyObject *PyMethod_Self(PyObject *im)
{
    if (!PyMethod_Check(im)) {
        PyErr_BadInternalCall();
        return NULL;
    }
    return ((PyMethodObject *)im)->im_self;
}

/* ---- instancemethod (CPython Objects/classobject.c), a C-API-only type -- */

static PyObject *qp_capi_instancemethod_getattro(PyObject *self, PyObject *name)
{
    PyTypeObject *tp = Py_TYPE(self);
    PyObject *descr = _PyType_LookupRef(tp, name), *res;
    if (descr) {
        descrgetfunc f = Py_TYPE(descr)->tp_descr_get;
        if (f)
            res = f(descr, self, (PyObject *)tp);
        else
            res = Py_NewRef(descr);
        Py_DECREF(descr);
        return res;
    }
    return PyObject_GetAttr(((PyInstanceMethodObject *)self)->func, name);
}

static void qp_capi_instancemethod_dealloc(PyObject *self)
{
    PyObject_GC_UnTrack(self);
    Py_DECREF(((PyInstanceMethodObject *)self)->func);
    PyObject_GC_Del(self);
}

static int qp_capi_instancemethod_traverse(PyObject *self, visitproc visit, void *arg)
{
    Py_VISIT(((PyInstanceMethodObject *)self)->func);
    return 0;
}

static PyObject *qp_capi_instancemethod_call(PyObject *self, PyObject *arg, PyObject *kw)
{
    return PyObject_Call(((PyInstanceMethodObject *)self)->func, arg, kw);
}

static PyObject *qp_capi_instancemethod_descr_get(PyObject *descr, PyObject *obj, PyObject *type)
{
    PyObject *func = ((PyInstanceMethodObject *)descr)->func;
    if (!obj)
        return Py_NewRef(func);
    return PyMethod_New(func, obj);
}

static PyObject *qp_capi_instancemethod_richcompare(PyObject *self, PyObject *other, int op)
{
    int eq;
    if ((op != Py_EQ && op != Py_NE) || !PyInstanceMethod_Check(self) ||
        !PyInstanceMethod_Check(other))
        Py_RETURN_NOTIMPLEMENTED;
    eq = PyObject_RichCompareBool(((PyInstanceMethodObject *)self)->func,
                                  ((PyInstanceMethodObject *)other)->func, Py_EQ);
    if (eq < 0)
        return NULL;
    return PyBool_FromLong(op == Py_EQ ? eq : !eq);
}

static PyObject *qp_capi_instancemethod_repr(PyObject *self)
{
    PyObject *func = ((PyInstanceMethodObject *)self)->func, *funcname, *res;
    const char *defname = "?";
    if (PyObject_GetOptionalAttrString(func, "__name__", &funcname) < 0)
        return NULL;
    if (funcname && !PyUnicode_Check(funcname))
        Py_CLEAR(funcname);
    res = PyUnicode_FromFormat("<instancemethod %V at %p>", funcname, defname, self);
    Py_XDECREF(funcname);
    return res;
}

static PyObject *qp_capi_instancemethod_get_doc(PyObject *self, void *closure)
{
    return PyObject_GetAttrString(((PyInstanceMethodObject *)self)->func, "__doc__");
}

static PyObject *qp_capi_instancemethod_new(PyTypeObject *type, PyObject *args, PyObject *kw)
{
    PyObject *func;
    if (kw && PyDict_Size(kw) > 0) {
        PyErr_SetString(PyExc_TypeError, "instancemethod() takes no keyword arguments");
        return NULL;
    }
    if (!PyArg_UnpackTuple(args, "instancemethod", 1, 1, &func))
        return NULL;
    if (!PyCallable_Check(func)) {
        PyErr_SetString(PyExc_TypeError, "first argument must be callable");
        return NULL;
    }
    return PyInstanceMethod_New(func);
}

static PyMemberDef qp_capi_instancemethod_members[] = {
    {"__func__", _Py_T_OBJECT, offsetof(PyInstanceMethodObject, func), Py_READONLY,
     "the function (or other callable) implementing a method"},
    {NULL}
};

static PyGetSetDef qp_capi_instancemethod_getset[] = {
    {"__doc__", qp_capi_instancemethod_get_doc, NULL, NULL},
    {NULL}
};

PyTypeObject PyInstanceMethod_Type = {
    .ob_base = PyVarObject_HEAD_INIT(&PyType_Type, 0)
    .tp_name = "instancemethod",
    .tp_basicsize = sizeof(PyInstanceMethodObject),
    .tp_dealloc = qp_capi_instancemethod_dealloc,
    .tp_repr = qp_capi_instancemethod_repr,
    .tp_call = qp_capi_instancemethod_call,
    .tp_getattro = qp_capi_instancemethod_getattro,
    .tp_setattro = PyObject_GenericSetAttr,
    .tp_flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC,
    .tp_doc = "instancemethod(function)\n--\n\nBind a function to a class.",
    .tp_traverse = qp_capi_instancemethod_traverse,
    .tp_richcompare = qp_capi_instancemethod_richcompare,
    .tp_members = qp_capi_instancemethod_members,
    .tp_getset = qp_capi_instancemethod_getset,
    .tp_descr_get = qp_capi_instancemethod_descr_get,
    .tp_new = qp_capi_instancemethod_new,
};

PyObject *PyInstanceMethod_New(PyObject *func)
{
    PyInstanceMethodObject *m = PyObject_GC_New(PyInstanceMethodObject, &PyInstanceMethod_Type);
    if (!m)
        return NULL;
    m->func = Py_NewRef(func);
    PyObject_GC_Track((PyObject *)m);
    return (PyObject *)m;
}

PyObject *PyInstanceMethod_Function(PyObject *im)
{
    if (!PyInstanceMethod_Check(im)) {
        PyErr_BadInternalCall();
        return NULL;
    }
    return ((PyInstanceMethodObject *)im)->func;
}

PyObject *PyClassMethod_New(PyObject *callable)
{
    JSContext *ctx = qp_capi.ctx;
    JSValue c = qp_capi_value(ctx, callable);
    if (JS_IsException(c))
        return NULL;
    return qp_capi_steal(ctx, qp_classmethod_new(ctx, c));
}

PyObject *PyStaticMethod_New(PyObject *callable)
{
    JSContext *ctx = qp_capi.ctx;
    JSValue c = qp_capi_value(ctx, callable);
    if (JS_IsException(c))
        return NULL;
    return qp_capi_steal(ctx, qp_staticmethod_new(ctx, c));
}

/* ========================================================================= */
/* Kind DESCR, descriptor constructors                                        */
/* ========================================================================= */

static size_t qp_capi_descr_kind_size(JSContext *ctx, JSValueConst v)
{
    size_t s = sizeof(PyMethodDescrObject);
    if (sizeof(PyMemberDescrObject) > s) s = sizeof(PyMemberDescrObject);
    if (sizeof(PyGetSetDescrObject) > s) s = sizeof(PyGetSetDescrObject);
    if (sizeof(PyWrapperDescrObject) > s) s = sizeof(PyWrapperDescrObject);
    return s - sizeof(PyObject);
}

static int qp_capi_descr_kind_init(JSContext *ctx, PyObject *op, JSValueConst v)
{
    QPDescr *d = JS_VALUE_GET_OBJ(v)->u.opaque;
    PyDescrObject *pd = (PyDescrObject *)op;
    QPType *oc;
    PyTypeObject *tp;

    if (!d) {
        qp_raise_system_error(ctx, "bad descriptor object");
        return -1;
    }
    QP_CAPI_HEAD(op)->flags |= QP_CAPI_F_PERSIST;
    oc = qp_type_from_value(d->objclass);
    if (oc) {
        tp = qp_capi_type_from_qp(ctx, oc);
        if (!tp)
            return -1;
        pd->d_type = (PyTypeObject *)Py_NewRef((PyObject *)tp);
    }
    pd->d_name = qp_capi_steal(ctx, qp_str_from_atom(ctx, d->name));
    if (!pd->d_name)
        return -1;
    switch (d->kind) {
    case QP_DESCR_METHOD:
    case QP_DESCR_CLASSMETHOD:
        ((PyMethodDescrObject *)op)->d_method = qp_capi_c_methoddef(d->u.method, NULL);
        if (!((PyMethodDescrObject *)op)->d_method)
            return -1;
        ((PyMethodDescrObject *)op)->vectorcall = qp_capi_mirror_vectorcall;
        break;
    case QP_DESCR_GETSET:
        ((PyGetSetDescrObject *)op)->d_getset = qp_capi_c_getsetdef(d->u.getset);
        if (!((PyGetSetDescrObject *)op)->d_getset)
            return -1;
        break;
    case QP_DESCR_MEMBER:
        if (d->u.member.getset && d->u.member.getset->get == qp_capi_member_get) {
            QPCapiDef *cd = qp_capi_def_at(d->u.member.getset->magic);
            ((PyMemberDescrObject *)op)->d_member = cd ? cd->u.m : NULL;
        }
        break;
    case QP_DESCR_WRAPPER:
    case QP_DESCR_METHOD_WRAPPER:
        ((PyWrapperDescrObject *)op)->d_wrapped = d->u.wrapper.wrapped;
        break;
    default:
        break;
    }
    return 0;
}

static int qp_capi_descr_kind_traverse(PyObject *op, visitproc visit, void *arg)
{
    PyDescrObject *pd = (PyDescrObject *)op;
    Py_VISIT((PyObject *)pd->d_type);
    Py_VISIT(pd->d_name);
    Py_VISIT(pd->d_qualname);
    return 0;
}

static void qp_capi_descr_kind_clear(JSRuntime *rt, PyObject *op)
{
    PyDescrObject *pd = (PyDescrObject *)op;
    PyObject *t = (PyObject *)pd->d_type;
    pd->d_type = NULL;
    Py_XDECREF(t);
    Py_CLEAR(pd->d_name);
    Py_CLEAR(pd->d_qualname);
}

/* unit h private constructor (python/descrobject.c) */
static JSValue qp_descr_new_classmethod(JSContext *ctx, QPType *t, const QPMethodDef *ml);

PyObject *PyDescr_NewMethod(PyTypeObject *type, PyMethodDef *method)
{
    JSContext *ctx = qp_capi.ctx;
    QPType *t = qp_capi_type_qp(type);
    const QPMethodDef *q;
    if (!t)
        return NULL;
    q = qp_capi_method_def(ctx, method, type);
    if (!q)
        return NULL;
    return qp_capi_steal(ctx, qp_descr_new_method(ctx, t, q));
}

PyObject *PyDescr_NewClassMethod(PyTypeObject *type, PyMethodDef *method)
{
    JSContext *ctx = qp_capi.ctx;
    QPType *t = qp_capi_type_qp(type);
    const QPMethodDef *q;
    if (!t)
        return NULL;
    q = qp_capi_method_def(ctx, method, type);
    if (!q)
        return NULL;
    return qp_capi_steal(ctx, qp_descr_new_classmethod(ctx, t, q));
}

PyObject *PyDescr_NewMember(PyTypeObject *type, PyMemberDef *member)
{
    JSContext *ctx = qp_capi.ctx;
    QPType *t = qp_capi_type_qp(type);
    const QPGetSetDef *q;
    if (!t)
        return NULL;
    q = qp_capi_member_def(ctx, member);
    if (!q)
        return NULL;
    return qp_capi_steal(ctx, qp_descr_new_member_getset(ctx, t, q));
}

PyObject *PyDescr_NewGetSet(PyTypeObject *type, PyGetSetDef *getset)
{
    JSContext *ctx = qp_capi.ctx;
    QPType *t = qp_capi_type_qp(type);
    const QPGetSetDef *q;
    if (!t)
        return NULL;
    q = qp_capi_getset_def(ctx, getset);
    if (!q)
        return NULL;
    return qp_capi_steal(ctx, qp_descr_new_getset(ctx, t, q));
}

/* PyDescr_NewWrapper: a method_descriptor calling base->wrapper(self, args,
   wrapped[, kwds]) (the runtime's wrapper_descriptor wraps runtime slots
   only; capi-design deviation) */
PyObject *PyDescr_NewWrapper(PyTypeObject *type, struct wrapperbase *base, void *wrapped)
{
    JSContext *ctx = qp_capi.ctx;
    QPType *t = qp_capi_type_qp(type);
    const QPMethodDef *q;
    if (!t)
        return NULL;
    q = qp_capi_wrapper_def(ctx, base, wrapped);
    if (!q)
        return NULL;
    return qp_capi_steal(ctx, qp_descr_new_method(ctx, t, q));
}

int PyDescr_IsData(PyObject *ob)
{
    return Py_TYPE(ob)->tp_descr_set != NULL;
}

PyObject *PyDictProxy_New(PyObject *mapping)
{
    JSContext *ctx = qp_capi.ctx;
    JSValue m = qp_capi_value(ctx, mapping);
    if (JS_IsException(m))
        return NULL;
    return qp_capi_from_js_result(ctx, qp_call1(ctx, QP_TYPE_VALUE(QP_TYPE(ctx, mappingproxy)), m));
}

PyObject *PyWrapper_New(PyObject *d, PyObject *self)
{
    JSContext *ctx = qp_capi.ctx;
    JSValue dj = qp_capi_value(ctx, d), sj;
    QPType *dt;
    if (JS_IsException(dj))
        return NULL;
    sj = qp_capi_value(ctx, self);
    if (JS_IsException(sj))
        return NULL;
    dt = qp_type_of(ctx, dj);
    if (!dt->slots.tp_descr_get) {
        PyErr_Format(PyExc_TypeError, "'%T' object is not a descriptor", d);
        return NULL;
    }
    return qp_capi_from_js_result(ctx, dt->slots.tp_descr_get(ctx, dj, sj,
                                                              QP_TYPE_VALUE(qp_type_of(ctx, sj))));
}

/* ========================================================================= */
/* Kind FUNCTION and the PyFunction_* API                                     */
/* ========================================================================= */

static size_t qp_capi_func_kind_size(JSContext *ctx, JSValueConst v)
{
    return sizeof(PyFunctionObject) - sizeof(PyObject);
}

static QPFuncData *qp_capi_func_data(JSContext *ctx, PyObject *op, JSValue *pv)
{
    JSValue v;
    QPFuncData *d;
    if (!op) {
        PyErr_BadInternalCall();
        return NULL;
    }
    v = qp_capi_value(ctx, op);
    if (JS_IsException(v))
        return NULL;
    d = qp_function_data(v);
    if (!d) {
        PyErr_BadInternalCall();
        return NULL;
    }
    if (pv)
        *pv = v;
    return d;
}

/* borrowed: value pinned in the function mirror under `role` (None -> NULL
   when none_is_null) */
static PyObject *qp_capi_func_borrow(JSContext *ctx, PyObject *op, int role, JSValueConst v,
                                     int none_is_null)
{
    if (QP_IS_NULL(v) || (none_is_null && JS_IsUndefined(v))) {
        qp_capi_pin_drop(op, QP_CAPI_PIN_NAMED(role));
        return NULL;
    }
    return qp_capi_borrow(ctx, op, QP_CAPI_PIN_NAMED(role), v);
}

static PyObject *qp_capi_func_borrow_attr(PyObject *op, int role, JSAtom atom, int none_is_null)
{
    JSContext *ctx = qp_capi.ctx;
    JSValue fv, v;
    PyObject *r;
    if (!qp_capi_func_data(ctx, op, &fv))
        return NULL;
    v = qp_getattr(ctx, fv, atom);
    if (JS_IsException(v))
        return NULL;
    r = qp_capi_func_borrow(ctx, op, role, v, none_is_null);
    JS_FreeValue(ctx, v);
    return r;
}

/* function watchers */
#define QP_CAPI_FUNC_MAX_WATCHERS 8
static PyFunction_WatchCallback qp_capi_func_watchers[QP_CAPI_FUNC_MAX_WATCHERS];

static_assert((int)PyFunction_EVENT_CREATE == QP_FUNC_EVENT_CREATE &&
              (int)PyFunction_EVENT_DESTROY == QP_FUNC_EVENT_DESTROY &&
              (int)PyFunction_EVENT_MODIFY_CODE == QP_FUNC_EVENT_MODIFY_CODE &&
              (int)PyFunction_EVENT_MODIFY_DEFAULTS == QP_FUNC_EVENT_MODIFY_DEFAULTS &&
              (int)PyFunction_EVENT_MODIFY_KWDEFAULTS == QP_FUNC_EVENT_MODIFY_KWDEFAULTS &&
              (int)PyFunction_EVENT_MODIFY_QUALNAME == QP_FUNC_EVENT_MODIFY_QUALNAME,
              "function watcher events");

static const char *qp_capi_func_event_name(PyFunction_WatchEvent event)
{
    switch (event) {
#define QP_CAPI_FUNC_EVENT_CASE(op) case PyFunction_EVENT_##op: return "PyFunction_EVENT_" #op;
    PY_FOREACH_FUNC_EVENT(QP_CAPI_FUNC_EVENT_CASE)
#undef QP_CAPI_FUNC_EVENT_CASE
    }
    return "?";
}

/* notify_func_watchers.  The events come from the runtime
   (qp_capi_func_event: def / lambda, FunctionType(), exec of code, the
   __code__ / __defaults__ / __kwdefaults__ / __qualname__ setters, which
   the PyFunction_Set* functions use too, and the deallocation through the
   finalizer queue) and from PyFunction_New*. */
static void qp_capi_func_notify(PyFunction_WatchEvent event, PyObject *func, PyObject *new_value)
{
    JSContext *ctx = qp_capi.ctx;
    unsigned bits = qp_capi_func_watchers_set;
    int i;
    for (i = 0; bits; i++, bits >>= 1) {
        PyFunction_WatchCallback cb;
        if (!(bits & 1))
            continue;
        cb = qp_capi_func_watchers[i];
        if (cb && cb(event, (PyFunctionObject *)func, new_value) < 0) {
            JSValue fv = qp_capi_value(ctx, func);
            QPFuncData *d = JS_IsException(fv) ? NULL : qp_function_data(fv);
            PyObject *qualname = d ? qp_capi_new_ref(ctx, d->qualname) : NULL;
            if (qualname)
                PyErr_FormatUnraisable("Exception ignored in %s watcher callback for "
                                       "function %U at %p", qp_capi_func_event_name(event),
                                       qualname, JS_VALUE_GET_PTR(fv));
            else
                PyErr_FormatUnraisable("Exception ignored in %s watcher callback",
                                       qp_capi_func_event_name(event));
            Py_XDECREF(qualname);
        }
    }
}

static void qp_capi_func_event(JSContext *ctx, int event, JSValueConst func,
                               JSValueConst new_value)
{
    PyObject *f, *v = NULL;

    if (!qp_capi_func_watchers_set || qp_capi_dead || !qp_capi.ctx || ctx != qp_capi.ctx ||
        qp_capi.initialized != 2)
        return;
    f = qp_capi_new_ref(ctx, func);
    if (f && !QP_IS_NULL(new_value) && !JS_IsUndefined(new_value) &&
        !(v = qp_capi_new_ref(ctx, new_value)))
        Py_CLEAR(f);
    if (!f) {
        PyErr_FormatUnraisable("Exception ignored in %s watcher callback",
                               qp_capi_func_event_name((PyFunction_WatchEvent)event));
        return;
    }
    qp_capi_func_notify((PyFunction_WatchEvent)event, f, v);
    Py_XDECREF(v);
    Py_DECREF(f);
}

int PyFunction_AddWatcher(PyFunction_WatchCallback callback)
{
    int i;
    for (i = 0; i < QP_CAPI_FUNC_MAX_WATCHERS; i++) {
        if (!qp_capi_func_watchers[i]) {
            qp_capi_func_watchers[i] = callback;
            qp_capi_func_watchers_set |= (uint8_t)(1 << i);
            return i;
        }
    }
    PyErr_SetString(PyExc_RuntimeError, "no more func watcher IDs available");
    return -1;
}

int PyFunction_ClearWatcher(int watcher_id)
{
    if (watcher_id < 0 || watcher_id >= QP_CAPI_FUNC_MAX_WATCHERS) {
        PyErr_Format(PyExc_ValueError, "invalid func watcher ID %d", watcher_id);
        return -1;
    }
    if (!qp_capi_func_watchers[watcher_id]) {
        PyErr_Format(PyExc_ValueError, "no func watcher set for ID %d", watcher_id);
        return -1;
    }
    qp_capi_func_watchers[watcher_id] = NULL;
    qp_capi_func_watchers_set &= (uint8_t)~(1 << watcher_id);
    return 0;
}

PyObject *PyFunction_NewWithQualName(PyObject *code, PyObject *globals, PyObject *qualname)
{
    JSContext *ctx = qp_capi.ctx;
    JSValue cj, gj, f;
    struct JSFunctionBytecode *b;
    PyObject *res;

    cj = qp_capi_value(ctx, code);
    if (JS_IsException(cj))
        return NULL;
    gj = qp_capi_value(ctx, globals);
    if (JS_IsException(gj))
        return NULL;
    b = qp_code_bytecode(cj);
    if (!b) {
        PyErr_BadInternalCall();
        return NULL;
    }
    if (!qp_dict_check(ctx, gj)) {
        PyErr_BadInternalCall();
        return NULL;
    }
    if (b->closure_var_count) {
        PyErr_SetString(PyExc_SystemError,
                        "PyFunction_New: code objects with free variables are not supported");
        return NULL;
    }
    if (!b->is_py) {
        PyErr_BadInternalCall();
        return NULL;
    }
    /* the qualname is set before PyFunction_EVENT_CREATE (no MODIFY event) */
    f = qp_fo_create_quiet(ctx, JS_MKPTR(JS_TAG_FUNCTION_BYTECODE, b), QP_NULL, gj, QP_NULL);
    if (JS_IsException(f))
        return NULL;
    if (qualname && qualname != Py_None) {
        JSValue q = qp_capi_value(ctx, qualname);
        QPFuncData *d = qp_function_data(f);
        if (JS_IsException(q)) {
            JS_FreeValue(ctx, f);
            return NULL;
        }
        if (!qp_str_check(ctx, q)) {
            JS_FreeValue(ctx, f);
            PyErr_SetString(PyExc_TypeError, "__qualname__ must be set to a string object");
            return NULL;
        }
        JS_FreeValue(ctx, d->qualname);
        d->qualname = JS_DupValue(ctx, q);
    }
    res = qp_capi_steal(ctx, f);
    if (res && qp_capi_func_watchers_set)
        qp_capi_func_notify(PyFunction_EVENT_CREATE, res, NULL);
    return res;
}

PyObject *PyFunction_New(PyObject *code, PyObject *globals)
{
    return PyFunction_NewWithQualName(code, globals, NULL);
}

PyObject *PyFunction_GetCode(PyObject *op)
{
    return qp_capi_func_borrow_attr(op, QP_CAPI_ROLE_FUNC_CODE, QP_ATOM(qp_capi.ctx, __code__), 0);
}

PyObject *PyFunction_GetGlobals(PyObject *op)
{
    JSContext *ctx = qp_capi.ctx;
    QPFuncData *d = qp_capi_func_data(ctx, op, NULL);
    return d ? qp_capi_func_borrow(ctx, op, QP_CAPI_ROLE_FUNC_GLOBALS, d->globals, 0) : NULL;
}

PyObject *PyFunction_GetModule(PyObject *op)
{
    JSContext *ctx = qp_capi.ctx;
    QPFuncData *d = qp_capi_func_data(ctx, op, NULL);
    return d ? qp_capi_func_borrow(ctx, op, QP_CAPI_ROLE_FUNC_MODULE, d->module, 0) : NULL;
}

PyObject *PyFunction_GetDefaults(PyObject *op)
{
    JSContext *ctx = qp_capi.ctx;
    QPFuncData *d = qp_capi_func_data(ctx, op, NULL);
    return d ? qp_capi_func_borrow(ctx, op, QP_CAPI_ROLE_FUNC_DEFAULTS, d->defaults, 1) : NULL;
}

PyObject *PyFunction_GetKwDefaults(PyObject *op)
{
    JSContext *ctx = qp_capi.ctx;
    QPFuncData *d = qp_capi_func_data(ctx, op, NULL);
    return d ? qp_capi_func_borrow(ctx, op, QP_CAPI_ROLE_FUNC_KWDEFAULTS, d->kwdefaults, 1) : NULL;
}

PyObject *PyFunction_GetClosure(PyObject *op)
{
    return qp_capi_func_borrow_attr(op, QP_CAPI_ROLE_FUNC_CLOSURE, QP_ATOM(qp_capi.ctx, __closure__), 1);
}

PyObject *PyFunction_GetAnnotations(PyObject *op)
{
    /* func_get_annotation_dict: None when there is neither a dict nor a
       callable __annotate__ (unlike the __annotations__ getter, no empty
       dict is created) */
    JSContext *ctx = qp_capi.ctx;
    QPFuncData *d = qp_capi_func_data(ctx, op, NULL);
    JSValue a;
    if (!d)
        return NULL;
    a = qp_fo_annotation_dict(ctx, d);
    if (JS_IsException(a))
        return NULL;
    if (JS_IsUndefined(a))
        return Py_None;
    return qp_capi_func_borrow(ctx, op, QP_CAPI_ROLE_FUNC_ANNOTATIONS, a, 1);
}

/* The setter of the attribute reports the MODIFY event, as CPython's
   PyFunction_Set* and the descriptors do. */
static int qp_capi_func_set(PyObject *op, JSAtom atom, PyObject *value)
{
    JSContext *ctx = qp_capi.ctx;
    JSValue fv, v = JS_UNDEFINED;
    if (!qp_capi_func_data(ctx, op, &fv))
        return -1;
    if (value) {
        v = qp_capi_value(ctx, value);
        if (JS_IsException(v))
            return -1;
    }
    return qp_setattr(ctx, fv, atom, v) < 0 ? -1 : 0;
}

int PyFunction_SetDefaults(PyObject *op, PyObject *defaults)
{
    if (defaults == Py_None)
        defaults = NULL;
    if (defaults && !PyTuple_Check(defaults)) {
        PyErr_SetString(PyExc_SystemError, "non-tuple default args");
        return -1;
    }
    return qp_capi_func_set(op, QP_ATOM(qp_capi.ctx, __defaults__), defaults);
}

int PyFunction_SetKwDefaults(PyObject *op, PyObject *defaults)
{
    if (defaults == Py_None)
        defaults = NULL;
    if (defaults && !PyDict_Check(defaults)) {
        PyErr_SetString(PyExc_SystemError, "non-dict keyword only default args");
        return -1;
    }
    return qp_capi_func_set(op, QP_ATOM(qp_capi.ctx, __kwdefaults__), defaults);
}

int PyFunction_SetAnnotations(PyObject *op, PyObject *annotations)
{
    if (annotations == Py_None)
        annotations = NULL;
    if (annotations && !PyDict_Check(annotations)) {
        PyErr_SetString(PyExc_SystemError, "non-dict annotations");
        return -1;
    }
    return qp_capi_func_set(op, QP_ATOM(qp_capi.ctx, __annotations__), annotations);
}

/* The cells of the tuple become the function's free variable references
   (python/funcobject.c qp_fo_set_closure); __closure__ returns the tuple. */
int PyFunction_SetClosure(PyObject *op, PyObject *closure)
{
    JSContext *ctx = qp_capi.ctx;
    JSValue fv, cv = JS_UNDEFINED;
    if (!qp_capi_func_data(ctx, op, &fv))
        return -1;
    if (closure == Py_None)
        closure = NULL;
    if (closure && !PyTuple_Check(closure)) {
        PyErr_Format(PyExc_SystemError, "expected tuple for closure, got '%.100s'",
                     Py_TYPE(closure)->tp_name);
        return -1;
    }
    if (closure) {
        cv = qp_capi_value(ctx, closure);
        if (JS_IsException(cv))
            return -1;
    }
    qp_fo_set_closure(ctx, fv, cv);
    return 0;
}

/* The runtime calls functions through their bytecode; a C vectorcall
   override is recorded in the mirror (PyFunctionObject layout) only. */
void PyFunction_SetVectorcall(PyFunctionObject *func, vectorcallfunc vectorcall)
{
    func->_qp_vectorcall = vectorcall;
}

/* ========================================================================= */
/* Struct sequences (bridged to python/structseq.c)                           */
/* ========================================================================= */

const char *const PyStructSequence_UnnamedField = QP_STRUCTSEQ_UNNAMED;

/* python/structseq.c private (defined earlier in the translation unit) */
static const QPStructSeqDesc *qp_ss_desc_of_type(JSContext *ctx, QPType *t);

static QPCapiPMap qp_capi_ss_descs;     /* PyStructSequence_Desc* -> QPStructSeqDesc* */

static const QPStructSeqDesc *qp_capi_ss_desc(PyStructSequence_Desc *desc)
{
    QPStructSeqDesc *q = qp_capi_pmap_get(&qp_capi_ss_descs, desc, NULL);
    const char **names, **docs;
    int i, n;
    if (q)
        return q;
    for (n = 0; desc->fields[n].name; n++)
        ;
    q = qp_sys_calloc(1, sizeof(*q));
    names = qp_sys_calloc(n + 1, sizeof(char *));
    docs = qp_sys_calloc(n + 1, sizeof(char *));
    if (!q || !names || !docs) {
        qp_sys_free(q);
        qp_sys_free(names);
        qp_sys_free(docs);
        PyErr_NoMemory();
        return NULL;
    }
    for (i = 0; i < n; i++) {
        names[i] = desc->fields[i].name == PyStructSequence_UnnamedField
            ? QP_STRUCTSEQ_UNNAMED : desc->fields[i].name;
        docs[i] = desc->fields[i].doc;
    }
    q->name = desc->name;
    q->doc = desc->doc;
    q->fields = names;
    q->field_docs = docs;
    q->n_in_sequence = desc->n_in_sequence;
    q->flags = 0;
    if (qp_capi_pmap_set(&qp_capi_ss_descs, desc, NULL, q) < 0) {
        qp_sys_free(q);
        qp_sys_free(names);
        qp_sys_free(docs);
        PyErr_NoMemory();
        return NULL;
    }
    return q;
}

PyTypeObject *PyStructSequence_NewType(PyStructSequence_Desc *desc)
{
    JSContext *ctx = qp_capi.ctx;
    const QPStructSeqDesc *q = qp_capi_ss_desc(desc);
    PyTypeObject *tp;
    JSValue tv;
    if (!q)
        return NULL;
    tv = qp_structseq_new_type(ctx, q);
    if (JS_IsException(tv))
        return NULL;
    tp = qp_capi_type_from_qp(ctx, qp_type_from_value(tv));
    JS_FreeValue(ctx, tv);
    return tp ? (PyTypeObject *)Py_NewRef((PyObject *)tp) : NULL;
}

int PyStructSequence_InitType2(PyTypeObject *type, PyStructSequence_Desc *desc)
{
    JSContext *ctx = qp_capi.ctx;
    const QPStructSeqDesc *q;
    QPCapiTypeInfo *ti;
    JSValue tv;
    int r;

    if (type->tp_flags & Py_TPFLAGS_READY) {
        PyErr_Format(PyExc_SystemError, "PyStructSequence_InitType2: type '%s' already initialized",
                     type->tp_name);
        return -1;
    }
    q = qp_capi_ss_desc(desc);
    if (!q)
        return -1;
    tv = qp_structseq_new_type(ctx, q);
    if (JS_IsException(tv))
        return -1;
    if (!Py_TYPE(type))
        Py_SET_TYPE(type, &PyType_Type);
    type->tp_name = desc->name;
    type->tp_doc = desc->doc;
    type->tp_basicsize = sizeof(PyStructSequence) - sizeof(PyObject *);
    type->tp_itemsize = sizeof(PyObject *);
    type->tp_flags |= Py_TPFLAGS_HAVE_GC;
    r = qp_capi_bind_static_type(ctx, type, qp_type_from_value(tv));
    ti = qp_capi_tinfo(type);
    if (ti && QP_IS_NULL(ti->j))
        ti->j = JS_DupValue(ctx, tv);
    JS_FreeValue(ctx, tv);
    return r;
}

void PyStructSequence_InitType(PyTypeObject *type, PyStructSequence_Desc *desc)
{
    if (PyStructSequence_InitType2(type, desc) < 0)
        Py_FatalError("PyStructSequence_InitType: failed to initialize type");
}

PyObject *PyStructSequence_New(PyTypeObject *type)
{
    JSContext *ctx = qp_capi.ctx;
    QPType *t = qp_capi_type_qp(type);
    const QPStructSeqDesc *d;
    JSValue *items, r;
    int n, i;
    if (!t)
        return NULL;
    d = qp_ss_desc_of_type(ctx, t);
    if (!d) {
        PyErr_Format(PyExc_TypeError, "'%s' is not a struct sequence type", type->tp_name);
        return NULL;
    }
    for (n = 0; d->fields[n]; n++)
        ;
    items = js_malloc(ctx, (n ? n : 1) * sizeof(JSValue));
    if (!items)
        return NULL;
    for (i = 0; i < n; i++)
        items[i] = JS_UNDEFINED;
    r = qp_structseq_new(ctx, t, items);
    js_free(ctx, items);
    return qp_capi_steal(ctx, r);
}

void PyStructSequence_SetItem(PyObject *op, Py_ssize_t i, PyObject *v)
{
    JSContext *ctx = qp_capi.ctx;
    JSValue j = qp_capi_value(ctx, op), nv;
    QPTuple *tup;
    if (JS_IsException(j) || !qp_tuple_check(ctx, j)) {
        Py_XDECREF(v);
        return;
    }
    nv = v ? qp_capi_to_js(ctx, v) : JS_UNDEFINED;
    Py_XDECREF(v);
    if (JS_IsException(nv))
        return;
    tup = QP_TUPLE(j);
    if (i >= 0 && i < (Py_ssize_t)tup->len) {
        JS_FreeValue(ctx, tup->items[i]);
        tup->items[i] = nv;
        tup->hash = -1;
    } else if (i >= 0 &&
               JS_DefinePropertyValueUint32(ctx, j, (uint32_t)i, nv, JS_PROP_WRITABLE) < 0) {
        return;
    }
    if (qp_capi.kind_ops[QP_CAPI_K_TUPLE].refresh)
        qp_capi.kind_ops[QP_CAPI_K_TUPLE].refresh(ctx, op);
}

PyObject *PyStructSequence_GetItem(PyObject *op, Py_ssize_t i)
{
    JSContext *ctx = qp_capi.ctx;
    JSValue j = qp_capi_value(ctx, op), v;
    QPTuple *tup;
    PyObject *r;
    if (JS_IsException(j) || !qp_tuple_check(ctx, j))
        return NULL;
    tup = QP_TUPLE(j);
    if (i >= 0 && i < (Py_ssize_t)tup->len)
        return PyTuple_GetItem(op, i);
    v = JS_GetPropertyUint32(ctx, j, (uint32_t)i);
    if (JS_IsException(v))
        return NULL;
    r = qp_capi_borrow(ctx, op, QP_CAPI_PIN_INDEX(i), v);
    JS_FreeValue(ctx, v);
    return r;
}

/* ========================================================================= */
/* types.GenericAlias, OrderedDict                                           */
/* ========================================================================= */

PyObject *Py_GenericAlias(PyObject *origin, PyObject *args)
{
    JSContext *ctx = qp_capi.ctx;
    JSValue o = qp_capi_value(ctx, origin), a;
    if (JS_IsException(o))
        return NULL;
    a = qp_capi_value(ctx, args);
    if (JS_IsException(a))
        return NULL;
    return qp_capi_from_js_result(ctx, qp_generic_alias_new(ctx, o, a));
}

/* PyODict_Type is the runtime's collections.OrderedDict
   (python/odictobject.c), also exported by _collections. */
PyObject *PyODict_New(void)
{
    JSContext *ctx = qp_capi.ctx;
    return qp_capi_from_js_result(ctx, qp_odict_new(ctx));
}

/* the JS value of an OrderedDict argument, JS_EXCEPTION otherwise */
static JSValue qp_capi_odict_value(JSContext *ctx, PyObject *od)
{
    JSValue v = qp_capi_value(ctx, od);
    if (JS_IsException(v))
        return v;
    if (!qp_odict_check(ctx, v)) {
        PyErr_BadInternalCall();
        return JS_EXCEPTION;
    }
    return v;
}

int PyODict_SetItem(PyObject *od, PyObject *key, PyObject *item)
{
    JSContext *ctx = qp_capi.ctx;
    JSValue d, k, v;

    d = qp_capi_odict_value(ctx, od);
    if (JS_IsException(d))
        return -1;
    k = qp_capi_value(ctx, key);
    if (JS_IsException(k))
        return -1;
    v = qp_capi_value(ctx, item);
    if (JS_IsException(v))
        return -1;
    return qp_odict_set_item(ctx, d, k, v);
}

int PyODict_DelItem(PyObject *od, PyObject *key)
{
    JSContext *ctx = qp_capi.ctx;
    JSValue d, k;

    d = qp_capi_odict_value(ctx, od);
    if (JS_IsException(d))
        return -1;
    k = qp_capi_value(ctx, key);
    if (JS_IsException(k))
        return -1;
    return qp_odict_del_item(ctx, d, k);
}

/* ========================================================================= */
/* Init and teardown                                                         */
/* ========================================================================= */

static int qp_capi_init_modules(JSContext *ctx);   /* capi_module.c */

/* the static builtin types; several can share one runtime type
   (PyCFunction_Type and PyCMethod_Type), so the type map does not list
   them all */
static const struct { PyTypeObject *tp; int id; } qp_capi_static_table[] = {
#define QP_CAPI_TAB(sym, id) { &sym, QP_T_##id },
    QP_CAPI_STATIC_TYPES(QP_CAPI_TAB)
#undef QP_CAPI_TAB
};

static int qp_capi_init_types(JSContext *ctx)
{
#define table qp_capi_static_table
    QPState *s = QP_STATE(ctx);
    size_t i;

    if (qp_capi_tinfo(&PyType_Type))
        return 0;
    qp_capi.type_map = &qp_capi_type_map_store;
    qp_capi.defs = &qp_capi_registry;
    for (i = 0; i < countof(qp_capi_slot_map); i++) {
        if (qp_capi_pmap_set(&qp_capi_fwd_set, qp_capi_slot_map[i].fwd, NULL, (void *)1) < 0) {
            PyErr_NoMemory();
            return -1;
        }
    }
    qp_capi_static_bases_init();

    qp_capi.kind_ops[QP_CAPI_K_TYPE] = (QPCapiKindOps){
        .name = "type", .size = qp_capi_type_kind_size, .init = qp_capi_type_kind_init,
        .to_js = qp_capi_type_kind_to_js, .refresh = qp_capi_type_kind_refresh,
        .traverse = qp_capi_type_kind_traverse, .clear = qp_capi_type_kind_clear,
        .gc_clear = qp_capi_type_kind_gc_clear };
    qp_capi.kind_ops[QP_CAPI_K_NATIVE] = (QPCapiKindOps){
        .name = "native", .size = qp_capi_native_kind_size, .init = qp_capi_native_kind_init,
        .to_js = qp_capi_native_kind_to_js, .refresh = qp_capi_native_kind_refresh };
    qp_capi.kind_ops[QP_CAPI_K_CFUNCTION] = (QPCapiKindOps){
        .name = "builtin_function_or_method", .size = qp_capi_cfunc_kind_size,
        .init = qp_capi_cfunc_kind_init, .traverse = qp_capi_cfunc_kind_traverse,
        .clear = qp_capi_cfunc_kind_clear };
    qp_capi.kind_ops[QP_CAPI_K_METHOD] = (QPCapiKindOps){
        .name = "method", .size = qp_capi_method_kind_size, .init = qp_capi_method_kind_init,
        .traverse = qp_capi_method_kind_traverse, .clear = qp_capi_method_kind_clear };
    qp_capi.kind_ops[QP_CAPI_K_FUNCTION] = (QPCapiKindOps){
        .name = "function", .size = qp_capi_func_kind_size };
    qp_capi.kind_ops[QP_CAPI_K_DESCR] = (QPCapiKindOps){
        .name = "descriptor", .size = qp_capi_descr_kind_size, .init = qp_capi_descr_kind_init,
        .traverse = qp_capi_descr_kind_traverse, .clear = qp_capi_descr_kind_clear };

    /* bind the static builtin types: register all, then complete */
    for (i = 0; i < countof(table); i++) {
        QPType *t = s->types[table[i].id];
        if (!t) {
            qp_raise_system_error(ctx, "runtime type of '%s' missing", table[i].tp->tp_name);
            return -1;
        }
        if (!qp_capi_static_register(ctx, table[i].tp, t, QP_CAPI_TO_BUILTIN))
            return -1;
    }
    for (i = 0; i < countof(table); i++) {
        if (qp_capi_static_complete(ctx, qp_capi_tinfo(table[i].tp)) < 0)
            return -1;
    }
    /* C-API-only types */
    if (PyType_Ready(&PyInstanceMethod_Type) < 0)
        return -1;
    return qp_capi_init_modules(ctx);
#undef table
}

static void qp_capi_types_teardown(JSContext *ctx)
{
    QPCapiPMap *m = &qp_capi_type_pmap;
    PyTypeObject **list;
    size_t i, n = 0;

    if (!m->tab)
        return;
    list = qp_sys_calloc(m->count + countof(qp_capi_static_table), sizeof(*list));
    if (!list)
        return;
    for (i = 0; i < m->size; i++) {
        QPCapiPMapEntry *e = &m->tab[i];
        if (e->a && e->a != QP_CAPI_PMAP_DELETED && n < m->count)
            list[n++] = e->value;
    }
    /* static types that alias another one's runtime type */
    for (i = 0; i < countof(qp_capi_static_table); i++) {
        PyTypeObject *tp = qp_capi_static_table[i].tp;
        QPCapiTypeInfo *ti = qp_capi_tinfo(tp);
        if (ti && ti->qt && qp_capi_type_map_get(ti->qt) != tp)
            list[n++] = tp;
    }
    for (i = 0; i < n; i++) {
        PyTypeObject *tp = list[i];
        QPCapiTypeInfo *ti = qp_capi_tinfo(tp);
        if (!ti || (ti->origin != QP_CAPI_TO_BUILTIN && ti->origin != QP_CAPI_TO_CSTATIC))
            continue;
        Py_CLEAR(ti->holder);
        if (!QP_CAPI_HAS_HEAD(tp)) {
            Py_CLEAR(tp->tp_dict);
            Py_CLEAR(tp->tp_bases);
            Py_CLEAR(tp->tp_mro);
        }
        JS_FreeValue(ctx, ti->j);
        ti->j = QP_NULL;
        tp->tp_flags &= ~(Py_TPFLAGS_READY | Py_TPFLAGS_READYING);
        qp_capi_tinfo_free(tp);
        /* back to the compiled-in state: the next binding readies it again */
        qp_capi_type_restore(tp);
    }
    qp_sys_free(list);
    qp_sys_free(m->tab);
    memset(m, 0, sizeof(*m));
    memset(qp_capi_dedicated_qt, 0, sizeof(qp_capi_dedicated_qt));
    /* the watchers are per interpreter (CPython clears them at finalization) */
    memset(qp_capi_type_watchers, 0, sizeof(qp_capi_type_watchers));
    memset(qp_capi_func_watchers, 0, sizeof(qp_capi_func_watchers));
    qp_capi_type_watchers_set = 0;
    qp_capi_func_watchers_set = 0;
}

/* ==== end of capi_type.c ==== */
