/*
 * dquickts Python C API (plan D5, Tier S): internal contract of the
 * implementation units.  Design rationale, ownership map and the symbol
 * inventory are in local://capi-design.md ("capi-design").
 *
 * Position in the build.  python/quickpy.c includes, after every M1/M2/M3
 * file and only when CONFIG_PYTHON_CAPI is defined (64-bit hosts):
 *
 *     #include "capi.h"
 *     #include "capi_mirror.c"      (A)   #include "capi_unicode.c"  (D)
 *     #include "capi_object.c"      (A)   #include "capi_bytes.c"    (D)
 *     #include "capi_type.c"        (B)   #include "capi_tuple.c"    (E1)
 *     #include "capi_module.c"      (B)   #include "capi_dict.c"     (E1)
 *     #include "capi_number.c"      (C)   #include "capi_errors.c"   (E1)
 *     #include "capi_abstract.c"    (C)   #include "capi_getargs.c"  (E2)
 *                                         #include "capi_runtime.c"  (E2)
 *
 * so this header sees python/object.h (QP*), quickjs.c internals and,
 * through capi/include/Python.h, the CPython 3.15 public API.  The C API
 * functions declared by Python.h (PyAPI_FUNC/PyAPI_DATA) are defined in
 * these files WITHOUT `static`: they are the exported symbols of
 * qpy/libquickjs (visibility default), linked by the CPython Modules/ *.c
 * translation units (capi/modules) and, from M5, by dlopen'ed abi3 modules.
 * Everything else in these files is `static` and named qp_capi_* / QPCapi*;
 * runtime-provided public-header helpers are named _PyQp_* (exported).
 *
 * ===================== CONVENTIONS (every C API unit) =====================
 * - One C-API interpreter per process: the JSContext given to
 *   qp_capi_init() (the first Python context that imports a C module).
 *   Exported functions find it in qp_capi.ctx; no exported function takes
 *   a JSContext.  Another context (or runtime, maybe in another thread)
 *   importing a C module gets ImportError "C extension modules are bound
 *   to another interpreter" until qp_capi_context_free released the
 *   binding (qp_capi_owner); it resets the interpreter-level C state, so
 *   a later runtime binds the C API again.
 * - CPython reference semantics at the C boundary: PyObject* results are
 *   new or borrowed exactly as documented by CPython 3.15; errors are the
 *   pending JS exception of qp_capi.ctx (PyErr_Occurred() <=> a pending
 *   exception), set with the object.h raising helpers (qp_raise...).
 * - JSValue <-> PyObject* crossings go ONLY through the section (A)
 *   functions below (qp_capi_new_ref, qp_capi_steal, qp_capi_borrow,
 *   qp_capi_value, qp_capi_to_js, qp_capi_from_js_result).
 * - Invariant: every PyObject* that C code can observe has ob_refcnt >= 1,
 *   owned by the observer (new reference) or by a pin of a live owner
 *   (borrowed reference), except inside tp_dealloc/tp_finalize/
 *   tp_traverse/tp_clear of that object.  Hence inline Py_INCREF never
 *   sees a 0 -> 1 transition and no hook is needed there; every 0 -> 1
 *   transition happens in qp_capi_incref() (A) and every 1 -> 0 in
 *   _Py_Dealloc() (A).
 * - Memory: object blocks come from qp_capi_alloc() (js_malloc_rt with a
 *   QPCapiHead prefix); PyMem_* are context-free (qp_sys_*); raw malloc/free
 *   do not compile in this TU.
 * - These files must not define macros named Py* / _Py* that leak: the
 *   public headers own those names here.
 */
#ifndef QP_CAPI_H
#define QP_CAPI_H

/* ---- namespace guard ------------------------------------------------------
   M1/M2 code may use CPython API names only as file-local macros #undef'd
   at the end of their file (m1-contract "Namespace rule").  A leaked macro
   would silently rewrite the public declarations below. */
#if defined(Py_None) || defined(Py_True) || defined(Py_False) ||            \
    defined(PyErr_Occurred) || defined(PyErr_NoMemory) ||                  \
    defined(PyExc_SyntaxError) || defined(PyExc_ValueError) ||             \
    defined(PyMem_Malloc) || defined(PyMem_Free) ||                        \
    defined(PyBytes_AS_STRING) || defined(PyThreadState_Get) ||            \
    defined(Py_UNREACHABLE) || defined(Py_ARRAY_LENGTH) ||                 \
    defined(Py_MAX) || defined(Py_MIN) || defined(Py_CHARMASK) ||          \
    defined(Py_TOLOWER) || defined(Py_TOUPPER) || defined(Py_ISDIGIT) ||   \
    defined(Py_SAFE_DOWNCAST) || defined(PyOS_strnicmp) ||                 \
    defined(Py_UCS1) || defined(Py_UCS2) || defined(Py_UCS4)
#error "python/capi.h: an M1/M2 file leaked a CPython API macro (namespace rule)"
#endif

/* TU-static M1/M2 functions whose names are exported C API symbols
   (CPython 3.15 exports them; the D5 modules link them).  The defining
   headers map them to qp_ names by macro (capi-design.md "Prerequisite
   edits"); the exported versions are defined by the owning C API unit as
   thin wrappers:
     unicodetype.h/.c   _PyUnicode_Is{Alpha,DecimalDigit,Digit,Linebreak,
                        Lowercase,Numeric,Printable,Titlecase,Uppercase,
                        Whitespace,XidStart,XidContinue},
                        _PyUnicode_To{DecimalDigit,Digit,Lowercase,Numeric,
                        Titlecase,Uppercase}, _Py_ctype_table,
                        _Py_ctype_tolower, _Py_ctype_toupper       -> (D)
     symtable.c         _Py_Mangle, _Py_IsPrivateName           -> (D) */
#undef _PyUnicode_IsAlpha
#undef _PyUnicode_IsDecimalDigit
#undef _PyUnicode_IsDigit
#undef _PyUnicode_IsLinebreak
#undef _PyUnicode_IsLowercase
#undef _PyUnicode_IsNumeric
#undef _PyUnicode_IsPrintable
#undef _PyUnicode_IsTitlecase
#undef _PyUnicode_IsUppercase
#undef _PyUnicode_IsWhitespace
#undef _PyUnicode_IsXidStart
#undef _PyUnicode_IsXidContinue
#undef _PyUnicode_ToDecimalDigit
#undef _PyUnicode_ToDigit
#undef _PyUnicode_ToLowercase
#undef _PyUnicode_ToNumeric
#undef _PyUnicode_ToTitlecase
#undef _PyUnicode_ToUppercase
#undef _Py_ctype_table
#undef _Py_ctype_tolower
#undef _Py_ctype_toupper
#undef _Py_Mangle
#undef _Py_IsPrivateName

#define QP_CAPI_IMPL 1
#include "../capi/include/Python.h"
#include "../capi/include/structmember.h"

/* ========================================================================= */
/* Mirror head (unit A)                                                       */
/* ========================================================================= */

/* Every dynamically allocated PyObject (mirror of a JS value, extension
   instance, PyObject_Malloc block) is preceded by a QPCapiHead in the same
   js_malloc_rt block.  Statically allocated objects (immortal: our builtin
   type objects and singletons, static types of extension modules) have no
   head: ob_flags & _Py_STATICALLY_ALLOCATED_FLAG.

   GC model (quickjs.c trial deletion, see capi-design.md "GC"):
   - A head is a GC node (gc_obj_type JS_GC_OBJ_TYPE_PY_CAPI, refcount and
     mark in its js_malloc block header) linked in rt->gc_obj_list exactly
     while QP_CAPI_F_LIVE (ob_refcnt > 0).
   - At the start of each collection qp_capi_gc_sync() sets the node's
     js_rc()->ref_count = ob_refcnt + (head bound to a JS object ? 1 : 0);
     the +1 is the edge "JS object -> its mirror".
   - Children of a live node: the bound JS object (owned while LIVE), the
     pins, the mirror-owned C references (kind ops traverse) and, for
     tracked extension instances, tp_traverse.
   - A JS object with JSObject.py_has_mirror visits its mirror: the node if
     LIVE, else (weak mirror) the mirror's children directly. */
typedef struct QPCapiHead {
    JSGCObjectHeader gc;        /* node link (gc_obj_list) while LIVE */
    struct list_head live_link; /* qp_capi.live_list while LIVE, qp_capi.weak_list while WEAK */
    JSValue v;                  /* JS value of the object; QP_NULL = unbound (C-primary, not crossed yet) */
    struct QPCapiPins *pins;    /* borrowed references handed out by this object, NULL = none */
    uint16_t kind;              /* QP_CAPI_K_*: layout after the PyObject header */
    uint16_t flags;             /* QP_CAPI_F_* */
    uint32_t tag;               /* QP_CAPI_TAG_*: MUST stay the last field (read at op - 4) */
} QPCapiHead;

/* _PyQp_HasFlag (capi/include/object.h) reads the flags word at op - 6 and
   tests the static flag as bit 2 of ob_flags; _Py_QP_BOUND_FLAG and
   _Py_QP_LIVE_SIZE_FLAG share the word with the QP_CAPI_F_* bits */
_Static_assert(offsetof(QPCapiHead, flags) == sizeof(QPCapiHead) - 6, "QPCapiHead.flags at op - 6");
_Static_assert(_Py_STATICALLY_ALLOCATED_FLAG == (1 << 2), "_PyQp_HasFlag static bit");

#define QP_CAPI_TAG_OBJECT 0x4f504351u  /* "QCPO": a PyObject follows */
#define QP_CAPI_TAG_RAW    0x57415251u  /* "QRAW": PyObject_Malloc block not (yet) an object */
#define QP_CAPI_TAG_FREED  0x45455246u  /* poison after release (debug) */

#define QP_CAPI_HEAD(op)  ((QPCapiHead *)((uint8_t *)(op) - sizeof(QPCapiHead)))
#define QP_CAPI_OBJ(h)    ((PyObject *)((uint8_t *)(h) + sizeof(QPCapiHead)))
#define QP_CAPI_HAS_HEAD(op) \
    (!(_PyObject_CAST(op)->ob_flags & _Py_STATICALLY_ALLOCATED_FLAG))

/* head flags */
#define QP_CAPI_F_LIVE      0x0001  /* ob_refcnt > 0: GC node, owns a JS reference when bound to an object */
#define QP_CAPI_F_OWNS_V    0x0002  /* head holds a counted JS reference to v */
#define QP_CAPI_F_PERSIST   0x0004  /* survives ob_refcnt == 0 while the JS object lives */
#define QP_CAPI_F_NATIVE    0x0008  /* extension instance: C payload is authoritative, tp_dealloc runs at the end */
#define QP_CAPI_F_TRACKED   0x0010  /* PyObject_GC_Track'ed: tp_traverse is called */
#define QP_CAPI_F_FINALIZED 0x0020  /* tp_finalize already ran (PEP 442) */
#define QP_CAPI_F_ORPHAN    0x0040  /* bound JS object freed by cycle collection while LIVE: v is dead */
#define QP_CAPI_F_INTERNED  0x0080  /* str mirror in qp_capi.str_map (identity while live) */
#define QP_CAPI_F_DEALLOC   0x0100  /* tp_dealloc in progress (re-entrancy guard) */
/* 0x1000 _Py_QP_BOUND_FLAG, 0x2000 _Py_QP_LIVE_SIZE_FLAG (capi/include/object.h,
   read by inline header code); 0x8000 QP_CAPI_F_LEAKED (capi_mirror.c) */

/* mirror kinds: the layout following PyObject.  Builtin kinds may also
   carry QP_CAPI_F_NATIVE when a C type extends the builtin (defaultdict:
   kind DICT + NATIVE, tp_basicsize > sizeof(PyDictObject)). */
#define QP_CAPI_KINDS(X)                                                     \
    X(RAW,        A)  /* PyObject_Malloc block (tag RAW) */                 \
    X(NATIVE,     B)  /* extension instance over object (JS class PY_NATIVE) */ \
    X(OBJECT,     A)  /* any other JS value: plain PyObject header */       \
    X(TYPE,       B)  /* PyHeapTypeObject mirror of a QPType */              \
    X(MODULE,     B)  /* PyModuleObject (md_def, md_state, md_dict, md_name) */ \
    X(CFUNCTION,  B)  /* PyCFunctionObject / PyCMethodObject */             \
    X(METHOD,     B)  /* PyMethodObject */                                  \
    X(FUNCTION,   B)  /* PyFunctionObject (size only) */                    \
    X(DESCR,      B)  /* PyDescrObject family (d_type, d_name, d_qualname) */ \
    X(LONG,       C)  /* PyLongObject with CPython digits (C-primary allowed) */ \
    X(FLOAT,      C)  /* PyFloatObject (C-primary allowed) */               \
    X(COMPLEX,    C)  /* PyComplexObject */                                 \
    X(STR,        D)  /* PyUnicodeObject (CPython layout; C-primary allowed) */ \
    X(BYTES,      D)  /* PyBytesObject (data in the JS ArrayBuffer) */      \
    X(BYTEARRAY,  D)  /* PyByteArrayObject (live size) */                   \
    X(MEMORYVIEW, D)  /* PyMemoryViewObject (view materialized) */          \
    X(TUPLE,      E1) /* PyTupleObject (ob_item materialized; C-primary allowed) */ \
    X(LIST,       E1) /* PyListObject (live size, item pins) */             \
    X(DICT,       E1) /* PyDictObject (value pins) */                       \
    X(SET,        E1) /* PySetObject (key pins) */                          \
    X(SLICE,      E1) /* PySliceObject (start/stop/step materialized) */    \
    X(EXCEPTION,  E1) /* PyBaseExceptionObject (size only) */               \
    X(CELL,       E1) /* PyCellObject */                                    \
    X(WEAKREF,    E1) /* PyWeakReference (size only) */                     \
    X(CODE,       E2) /* PyCodeObject (scalar co_* fields materialized) */

typedef enum QPCapiKind {
#define QP_CAPI_KIND_ENUM(id, owner) QP_CAPI_K_ ## id,
    QP_CAPI_KINDS(QP_CAPI_KIND_ENUM)
#undef QP_CAPI_KIND_ENUM
    QP_CAPI_K__COUNT,
} QPCapiKind;

/* Per-kind behaviour, registered by the owner unit in its init function.
   All hooks are optional. */
typedef struct QPCapiKindOps {
    const char *name;
    /* bytes after the PyObject header for value v (default: the fixed size
       of the public struct minus sizeof(PyObject)) */
    size_t (*size)(JSContext *ctx, JSValueConst v);
    /* fill the fields of a new mirror of v (head->v set and owned).  May
       create child mirrors (owned references or pins).  0 or -1 + error. */
    int (*init)(JSContext *ctx, PyObject *op, JSValueConst v);
    /* C-primary objects (unbound): build the JS value at first crossing
       (new reference).  The mirror is then bound to it. */
    JSValue (*to_js)(JSContext *ctx, PyObject *op);
    /* re-read mutable mirrored facts before a hand-out (e.g. ob_type after
       __class__ assignment, tp_flags of a type mirror). */
    void (*refresh)(JSContext *ctx, PyObject *op);
    /* visit the C references owned by the mirror fields (not pins, not
       tp_traverse): tuple items, cfunction m_self/m_module, ... */
    int (*traverse)(PyObject *op, visitproc visit, void *arg);
    /* release the C references/buffers owned by the mirror fields (runs
       without a context: only Py_DECREF/qp_sys_free/js_free_rt) */
    void (*clear)(JSRuntime *rt, PyObject *op);
    /* the cycle collector's clear (default: clear): break the cycles but
       keep what the tp_clear of the other garbage still reads, as
       CPython's type_clear keeps tp_base (PyObject_GetTypeData of a
       metaclass instance); clear releases the rest at dealloc */
    void (*gc_clear)(JSRuntime *rt, PyObject *op);
} QPCapiKindOps;

/* Borrowed-reference pins (unit A).  An owner mirror keeps one counted
   reference per handed-out borrowed object, keyed by a slot key; a pin is
   replaced when its slot is read again and holds another value, and all
   pins are released with the owner mirror.  Keys: */
#define QP_CAPI_PIN_INDEX(i)   ((uint64_t)(i))                       /* list/tuple-like index */
#define QP_CAPI_PIN_NAMED(n)   (((uint64_t)2 << 62) | (uint64_t)(n)) /* fixed role (QP_CAPI_ROLE_*) */
#define QP_CAPI_PIN_HASHED(h)  (((uint64_t)1 << 62) | ((uint64_t)(h) & (((uint64_t)1 << 62) - 1)))  /* dict key/value by key hash+identity */
enum {
    QP_CAPI_ROLE_TYPE_DICT, QP_CAPI_ROLE_TYPE_BASES, QP_CAPI_ROLE_TYPE_MRO,
    QP_CAPI_ROLE_TYPE_BASE, QP_CAPI_ROLE_MODULE_DICT, QP_CAPI_ROLE_MODULE_NAME,
    QP_CAPI_ROLE_FUNC_CODE, QP_CAPI_ROLE_FUNC_GLOBALS, QP_CAPI_ROLE_FUNC_MODULE,
    QP_CAPI_ROLE_FUNC_DEFAULTS, QP_CAPI_ROLE_FUNC_KWDEFAULTS,
    QP_CAPI_ROLE_FUNC_CLOSURE, QP_CAPI_ROLE_FUNC_ANNOTATIONS,
    QP_CAPI_ROLE_CELL, QP_CAPI_ROLE_WEAKREF, QP_CAPI_ROLE_EXC_TYPE,
    QP_CAPI_ROLE_EXC_VALUE, QP_CAPI_ROLE_EXC_TB, QP_CAPI_ROLE_TSTATE_DICT,
    QP_CAPI_ROLE_BUILTINS, QP_CAPI_ROLE_GLOBALS, QP_CAPI_ROLE_SYS_ATTR,
    QP_CAPI_ROLE__COUNT
};
typedef struct QPCapiPins QPCapiPins;   /* private to unit A */

/* Process-wide C-API state (unit A owns the struct; each unit owns the
   fields of its block and initializes them in its init function). */
typedef struct QPCapiState {
    /* (A) */
    JSContext *ctx;                 /* the C-API interpreter (NULL before qp_capi_init) */
    JSRuntime *rt;
    struct list_head live_list;     /* live heads (refcount sync pass) */
    struct list_head weak_list;     /* weak heads (bound, ob_refcnt dropped to 0): revived by the sync pass
                                       when C took a reference back with an inline Py_INCREF */
    struct QPCapiMap *obj_map;      /* JSObject* -> head (objects with py_has_mirror) */
    struct QPCapiMap *str_map;      /* JSString* -> live str head (identity of live str mirrors) */
    struct QPCapiMap *id_map;       /* _Py_ID / _Py_STR / PyUnicode_InternFromString: name -> immortal interned str mirror */
    struct QPCapiMap *static_map;   /* statically allocated extension instance -> its PY_NATIVE JSObject* (owned) */
    QPCapiKindOps kind_ops[QP_CAPI_K__COUNT];
    int initialized;                /* 0 none, 1 in progress, 2 ready */
    /* (B) */
    struct QPCapiTypeMap *type_map; /* B-private: QPType* -> bound static PyTypeObject* (reverse: tp_cache) */
    struct QPCapiRegistry *defs;    /* PyMethodDef/PyGetSetDef/PyMemberDef registry: QP magic -> C def */
    struct _inittab *inittab;       /* PyImport_ExtendInittab/AppendInittab tables (B owns the registry) */
    QPModuleDef **qp_moddefs;       /* synthesized inittab entries (B; each entry allocated once, stable) */
    int qp_moddef_count;
    /* (D) */
    struct QPCapiCodecState *codecs;
    /* (E1) */
    PyObject *exc_pins;             /* holder mirror of PyErr_Occurred()/PyErr_GetRaisedException borrowed types */
    /* (E2) */
    PyThreadState *tstate_current;  /* NULL between PyEval_SaveThread and PyEval_RestoreThread */
    struct QPCapiThreadState *main_tstate;
    PyInterpreterState *interp;
} QPCapiState;

static QPCapiState qp_capi;

/* ========================================================================= */
/* (A) MIRROR CORE: python/capi_mirror.c, python/capi_object.c                */
/* ========================================================================= */

/* Bind the C API to ctx and run the unit inits in order: A
   (qp_capi_init_mirror), B (qp_capi_init_types), C (qp_capi_init_numbers),
   D (qp_capi_init_str), E1 (qp_capi_init_containers, qp_capi_init_errors),
   E2 (qp_capi_init_runtime).  Idempotent for the bound context; called by
   the inittab bridge (B) before the first PyInit_* runs.  0 / -1. */
static int qp_capi_init(JSContext *ctx);
static int qp_capi_init_mirror(JSContext *ctx);

/* -- quickjs.c hooks (calls added by M2Interp under CONFIG_PYTHON_CAPI; the
      prototypes are repeated in object.h's C-API hook section) -- */
/* gc_decref() start: node refcounts := ob_refcnt + bound. */
static void qp_capi_gc_sync(JSRuntime *rt);
/* mark_children() case JS_GC_OBJ_TYPE_PY_CAPI. */
static void qp_capi_mark_node(JSRuntime *rt, JSGCObjectHeader *gp, JS_MarkFunc *mark_func);
/* mark_children() JS_GC_OBJ_TYPE_JS_OBJECT when p->py_has_mirror. */
static void qp_capi_mark_object(JSRuntime *rt, JSObject *p, JS_MarkFunc *mark_func);
/* gc_free_cycles() case JS_GC_OBJ_TYPE_PY_CAPI: garbage node; calls
   tp_clear (natives) / kind clear, which drops C references until the node
   is released through _Py_Dealloc. */
static void qp_capi_free_cycle_node(JSRuntime *rt, JSGCObjectHeader *gp);
/* free_object() before the class finalizer when p->py_has_mirror: weak
   mirror -> released (tp_dealloc for natives); live mirror -> ORPHAN. */
static void qp_capi_object_freed(JSRuntime *rt, JSObject *p);
/* __JS_FreeValueRT of an object with py_has_mirror whose last JS reference
   is gone: TRUE when a bare Py_INCREF revived its weak mirror, which then
   owns J again (J survives). */
static BOOL qp_capi_object_revived(JSRuntime *rt, JSObject *p);
/* JS_FreeContext of the C-API context (before QPState is freed): drops the
   JS references of every live node and unlinks the nodes (C-side memory
   still referenced by extension globals is leaked), resets the
   interpreter-level C state (static types back to their compiled-in
   state, interned strings, thread states...) and releases the binding. */
static void qp_capi_context_free(JSContext *ctx);
static void qp_capi_runtime_free(JSRuntime *rt);
/* gc_free_cycles() final loop: a PY_CAPI node still listed after the free
   phase (tp_traverse misreported) is unlinked and leaked. */
static void qp_capi_leak_node(JSRuntime *rt, JSGCObjectHeader *gp);

/* -- allocation -- */
/* New object block: head (tag OBJECT, kind) + an object of `size` bytes
   TOTAL, PyObject header included (like tp_basicsize; raised to
   sizeof(PyObject) if smaller), zeroed, ob_refcnt = 1 (LIVE, linked),
   ob_type = type (a counted reference when the type has a head).  Kind
   NATIVE also gets QP_CAPI_F_NATIVE.  NULL + MemoryError. */
static PyObject *qp_capi_alloc(PyTypeObject *type, size_t size, QPCapiKind kind);
/* Release the block of a head whose object is dead (unlinks map entries,
   pins, node). */
static void qp_capi_release(JSRuntime *rt, QPCapiHead *h);

/* -- reference transitions -- */
/* Counted reference with the 0 -> 1 transition (LIVE: link node, take the
   JS reference).  Use for every hand-out of a possibly weak mirror. */
static void qp_capi_incref(PyObject *op);
/* (_Py_Dealloc, exported) 1 -> 0: mirrors drop their JS reference and are
   released unless PERSIST; unbound natives run tp_dealloc; bound natives
   wait for their JS object (qp_capi_object_freed). */

/* -- C method calls in progress (Py_REFCNT of their arguments) --
   CPython 3.14 passes self and the vectorcall arguments borrowed, and loads
   locals with LOAD_FAST_BORROW, so `a.resize(n)` on a function local sees
   Py_REFCNT(a) == 1.  The bridge holds references of its own and the QuickJS
   operand stack always counts its copies; _PyQp_RefcntBound leaves out those
   that CPython borrows (capi_mirror.c qp_capi_calls_borrowed). */
typedef struct QPCapiCall {
    struct QPCapiCall *prev;
    PyObject *self;             /* bridge reference of self, NULL if none */
    PyObject *const *args;      /* bridge references of the n arguments */
    int n;
    JSStackFrame *sf;           /* the calling Python frame when `slots` lie in its operand stack, else NULL */
    JSValueConst *slots;        /* the arguments as passed (slots[-1] = self when self_slot) */
    BOOL self_slot;             /* slots[-1] is the operand stack copy of self */
} QPCapiCall;
static void qp_capi_call_enter(JSContext *ctx, QPCapiCall *c, PyObject *self, PyObject *const *args,
                               JSValueConst self_v, JSValueConst *slots, int n);
static void qp_capi_call_leave(QPCapiCall *c);

/* -- crossings JS -> C -- */
/* New reference to the mirror of v (singletons, small ints and builtin
   types are static).  NULL + exception on failure. */
static PyObject *qp_capi_new_ref(JSContext *ctx, JSValueConst v);
/* Same, consuming v (freed even on failure). */
static PyObject *qp_capi_steal(JSContext *ctx, JSValue v);
/* Borrowed mirror of v, pinned in `owner` under `key` (see PIN keys):
   valid while owner's mirror lives and the slot keeps that value. */
static PyObject *qp_capi_borrow(JSContext *ctx, PyObject *owner, uint64_t key, JSValueConst v);
/* Convert a JS call result (consumed) to a C result: JS_EXCEPTION -> NULL
   (exception stays pending), else qp_capi_steal. */
static PyObject *qp_capi_from_js_result(JSContext *ctx, JSValue r);
/* The mirror of a JS object if one exists (borrowed), else NULL. */
static PyObject *qp_capi_lookup(JSValueConst v);
/* Generic J-backed mirror of kind `kind` for the JS object v (no kind
   dispatch, no lookup; used by B for TYPE mirrors of Python-defined
   types): allocates kind_ops[kind].size, binds (obj_map, py_has_mirror,
   _Py_QP_BOUND_FLAG, owned JS reference), ob_type = ob_type (counted),
   runs kind_ops[kind].init.  New reference or NULL + exception. */
static PyObject *qp_capi_new_mirror(JSContext *ctx, JSValueConst v, QPCapiKind kind,
                                    PyTypeObject *ob_type);
/* Make a bound mirror C-primary again (_PyQp_TupleSetItem when only the
   mirror references its JS value): clears _Py_QP_BOUND_FLAG, the obj_map
   entry and py_has_mirror, releases the owned head->v (v = QP_NULL). */
static void qp_capi_unbind(PyObject *op);

/* -- crossings C -> JS -- */
/* Borrowed JS value of op, valid while op is alive.  Binds a C-primary
   object first (may allocate): JS_EXCEPTION with a pending error on
   failure. */
static JSValue qp_capi_value(JSContext *ctx, PyObject *op);
/* New JS reference (qp_capi_value + dup). */
static JSValue qp_capi_to_js(JSContext *ctx, PyObject *op);
/* Arrays: new JS references of n objects into out[] (JS_UNDEFINED filled
   on failure, all freed); unit A API for the call bridges, currently
   without a user. */
static __maybe_unused int qp_capi_to_js_array(JSContext *ctx, PyObject *const *objs, Py_ssize_t n,
                                              JSValue *out);

/* -- pins -- */
static PyObject *qp_capi_pin_get(PyObject *owner, uint64_t key);               /* borrowed or NULL */
static void qp_capi_pin_set(PyObject *owner, uint64_t key, PyObject *obj);     /* steals obj, releases a replaced pin */
static void qp_capi_pin_drop(PyObject *owner, uint64_t key);
static void qp_capi_pins_visit(QPCapiPins *pins, visitproc visit, void *arg);
static void qp_capi_pins_free(JSRuntime *rt, QPCapiPins *pins);

/* -- identifiers: _Py_ID(name) / _Py_STR(name) / _Py_LATIN1_CHR(c) and
      PyUnicode_InternFromString: immortal interned str mirrors (A owns the
      table, D builds the str) -- */
static PyObject *qp_capi_intern_id(const char *name);
/* Exported targets of the capi/compat/pycore_global_strings.h and
   pycore_runtime.h shims (borrowed immortal objects):
     _Py_ID(NAME)          -> (*_PyQp_InternId(#NAME))
     _Py_STR(NAME)         -> (*_PyQp_InternStr(<CPython literal of NAME>))
     _Py_LATIN1_CHR(CH)    -> (*_PyQp_Latin1Char(CH))
     _Py_SINGLETON(tuple_empty) / (bytes_empty)
                           -> (*_PyQp_Singleton(QP_CAPI_SINGLETON_*)) */
enum { QP_CAPI_SINGLETON_TUPLE_EMPTY, QP_CAPI_SINGLETON_BYTES_EMPTY,
       QP_CAPI_SINGLETON_STR_EMPTY, QP_CAPI_SINGLETON__COUNT };
PyAPI_FUNC(PyObject *) _PyQp_InternId(const char *name);
PyAPI_FUNC(PyObject *) _PyQp_InternStr(const char *literal);
PyAPI_FUNC(PyObject *) _PyQp_Latin1Char(int ch);
PyAPI_FUNC(PyObject *) _PyQp_Singleton(int which);

/* -- calls (A, capi_object.c) -- */
/* Call a JS/Python callable from C with vectorcall arguments (borrowed
   mirrors), kwnames tuple mirror or NULL; new reference or NULL. */
static PyObject *qp_capi_vectorcall(PyObject *callable, PyObject *const *args,
                                    size_t nargsf, PyObject *kwnames);
/* _Py_CheckFunctionResult: SystemError "<callable> returned NULL without
   setting an exception" / "returned a result with an exception set". */
static PyObject *qp_capi_check_result(PyObject *callable, PyObject *result, const char *where);

/* ========================================================================= */
/* (B) TYPES, METHODS, DESCRIPTORS, MODULES: python/capi_type.c,              */
/*     python/capi_module.c                                                   */
/* ========================================================================= */

static int qp_capi_init_types(JSContext *ctx);

/* Static PyTypeObject of every builtin type the public headers declare,
   bound to the QPType of qp_capi.ctx at init (tp_cache = QPType*, slots =
   reverse bridges, tp_flags from QPType.flags).  X(c_symbol, QPTypeId). */
#define QP_CAPI_STATIC_TYPES(X)                                              \
    X(PyBaseObject_Type, object) X(PyType_Type, type) X(PySuper_Type, super) \
    X(PyProperty_Type, property) X(PyClassMethod_Type, classmethod)          \
    X(PyStaticMethod_Type, staticmethod)                                     \
    X(PyCFunction_Type, builtin_function_or_method)                          \
    X(PyCMethod_Type, builtin_function_or_method)                            \
    X(PyMethodDescr_Type, method_descriptor)                                 \
    X(PyClassMethodDescr_Type, classmethod_descriptor)                       \
    X(PyGetSetDescr_Type, getset_descriptor)                                 \
    X(PyMemberDescr_Type, member_descriptor)                                 \
    X(PyWrapperDescr_Type, wrapper_descriptor)                               \
    X(_PyNone_Type, NoneType)                                                \
    X(_PyNotImplemented_Type, NotImplementedType)                            \
    X(PyFunction_Type, function) X(PyMethod_Type, method)                    \
    X(PyCode_Type, code) X(PyCell_Type, cell) X(PyFrame_Type, frame)         \
    X(PyLong_Type, int) X(PyBool_Type, bool) X(PyFloat_Type, float)          \
    X(PyComplex_Type, complex) X(PyUnicode_Type, str) X(PyList_Type, list)   \
    X(PyTuple_Type, tuple) X(PyRange_Type, range) X(PySlice_Type, slice)     \
    X(PyEllipsis_Type, ellipsis) X(PyListIter_Type, list_iterator)           \
    X(PyListRevIter_Type, list_reverseiterator)                              \
    X(PyTupleIter_Type, tuple_iterator) X(PyRangeIter_Type, range_iterator)  \
    X(PyLongRangeIter_Type, longrange_iterator)                              \
    X(PyUnicodeIter_Type, str_iterator) X(PyBytesIter_Type, bytes_iterator)  \
    X(PyByteArrayIter_Type, bytearray_iterator)                              \
    X(PySeqIter_Type, iterator) X(PyCallIter_Type, callable_iterator)        \
    X(PyDict_Type, dict) X(PyFrozenDict_Type, frozendict)                    \
    X(PyDictKeys_Type, dict_keys) X(PyDictValues_Type, dict_values)          \
    X(PyDictItems_Type, dict_items) X(PyDictIterKey_Type, dict_keyiterator)  \
    X(PyDictIterValue_Type, dict_valueiterator)                              \
    X(PyDictIterItem_Type, dict_itemiterator)                                \
    X(PyDictRevIterKey_Type, dict_reversekeyiterator)                        \
    X(PyDictRevIterValue_Type, dict_reversevalueiterator)                    \
    X(PyDictRevIterItem_Type, dict_reverseitemiterator)                      \
    X(PyDictProxy_Type, mappingproxy) X(PyODict_Type, OrderedDict)           \
    X(PyODictKeys_Type, odict_keys) X(PyODictValues_Type, odict_values)      \
    X(PyODictItems_Type, odict_items) X(PyODictIter_Type, odict_iterator)    \
    X(PySet_Type, set)                                                       \
    X(PyFrozenSet_Type, frozenset) X(PySetIter_Type, set_iterator)           \
    X(PyBytes_Type, bytes) X(PyByteArray_Type, bytearray)                    \
    X(PyMemoryView_Type, memoryview) X(PyTraceBack_Type, traceback)          \
    X(PyGen_Type, generator) X(PyCoro_Type, coroutine)                       \
    X(PyAsyncGen_Type, async_generator) X(PyEnum_Type, enumerate)            \
    X(PyReversed_Type, reversed) X(PyFilter_Type, filter) X(PyMap_Type, map) \
    X(PyZip_Type, zip) X(Py_GenericAliasType, GenericAlias)                  \
    X(PySentinel_Type, sentinel)                                             \
    X(_PyAsyncGenASend_Type, async_generator_asend)                          \
    X(PyModule_Type, module)
/* Other PyAPI_DATA types of the public headers:
   - C-API-only types implemented by the capi units as static extension
     types (readied like any extension type, instances of JS class
     PY_NATIVE): PyCapsule_Type (E1), PyInstanceMethod_Type,
     PyModuleDef_Type (B), PyPickleBuffer_Type (D), PyStdPrinter_Type
     (python/capi_stable.c: PyFile_NewStdPrinter);
   - types of runtime modules, bound when that module is first imported by
     qp_capi_init_runtime / qp_capi_init_containers: PyContext_Type,
     PyContextVar_Type, PyContextToken_Type (E2, module _contextvars),
     _PyWeakref_RefType, _PyWeakref_ProxyType, _PyWeakref_CallableProxyType
     (E1, module _weakref);
   - never instantiated here (defined, immortal, so the *_Check macros are
     false): PyFrameLocalsProxy_Type (B owns this definition). */

/* PyTypeObject* <-> QPType* (static types: tp_cache; heap/extension types:
   tp_cache too).  qp_capi_type_from_qp returns a borrowed type mirror
   (created and pinned on first use; persists with the type). */
static QPType *qp_capi_type_qp(PyTypeObject *tp);
static PyTypeObject *qp_capi_type_from_qp(JSContext *ctx, QPType *t);
/* CPython's `Py_TYPE(o)->tp_as_buffer->bf_releasebuffer != NULL` seen
   through the reverse buffer bridge (capi_bytes.c). */
static int qp_capi_buffer_needs_release(PyObject *o);
/* Mirror type of a JS value: Py_TYPE() of its mirror (borrowed). */
static PyTypeObject *qp_capi_type_of(JSContext *ctx, JSValueConst v);
/* Hook called by qp_type_modified (unit h, request R5 of capi-design.md):
   refresh tp_flags (and cached slot pointers) of t's PyTypeObject. */
static void qp_capi_type_modified(JSContext *ctx, QPType *t);
/* Bind a statically allocated PyTypeObject of a builtin QP type (static
   types of B, exception classes of E1): tp_cache, type_map, tp_flags from
   QPType.flags (C-only bits preset by the caller are kept), NULL slots ->
   reverse bridges, tp_alloc/tp_free/tp_dealloc defaults, READY.  The
   caller sets ob_base, tp_name, tp_basicsize, tp_base.  Idempotent. 0/-1. */
static int qp_capi_bind_static_type(JSContext *ctx, PyTypeObject *tp, QPType *t);
/* Called first by qp_capi_context_free (A): releases the JS references B
   owns (J of static types in type_map, registry wrappers). */
static void qp_capi_types_teardown(JSContext *ctx);

/* Forward slot bridges (QPTypeSlots of C-defined types -> C slots) and
   reverse bridges (C slots of static builtin / Python-defined types ->
   QPTypeSlots) are private to B; slot inheritance follows CPython
   inherit_slots on the C side and qp_type_ready on the QP side. */

/* Method/getset/member defs registry: QP magic index -> C def.  The QP
   wrappers (QPMethodDef / QPGetSetDef) live as long as the process. */
static const QPMethodDef *qp_capi_method_def(JSContext *ctx, PyMethodDef *ml, PyTypeObject *defining_class);
static const QPGetSetDef *qp_capi_getset_def(JSContext *ctx, PyGetSetDef *gs);
static const QPGetSetDef *qp_capi_member_def(JSContext *ctx, PyMemberDef *m);

/* Inittab bridge: builtin C modules registered with PyImport_ExtendInittab
   appear to qp_inittab_find()/count()/get() (python/config.c consults
   these when its own table has no entry).  The synthesized QPModuleDef has
   methods NULL and exec = qp_capi_module_exec, which binds the C API
   (qp_capi_init), runs PyInit_<name> / PyModExport_<name>, and executes the
   PyModuleDef (multi-phase: m_size state, m_methods, Py_mod_exec slots;
   single-phase: copies the returned module's dict) into `module`. */
static const QPModuleDef *qp_capi_inittab_find(const char *name, size_t len);
static int qp_capi_inittab_count(void);
static const QPModuleDef *qp_capi_inittab_get(int i);
static int qp_capi_module_exec(JSContext *ctx, JSValueConst module);

/* ========================================================================= */
/* (C) NUMBERS AND ABSTRACT PROTOCOLS: python/capi_number.c,                  */
/*     python/capi_abstract.c                                                 */
/* ========================================================================= */

static int qp_capi_init_numbers(JSContext *ctx); /* small ints, bool singletons, kind ops LONG/FLOAT/COMPLEX */
/* Static immortal ints -_PY_NSMALLNEGINTS .. _PY_NSMALLPOSINTS-1. */
static PyObject *qp_capi_small_int(long v);     /* NULL if out of range */

/* ========================================================================= */
/* (D) STR, BYTES, BUFFERS: python/capi_unicode.c, python/capi_bytes.c        */
/* ========================================================================= */

static int qp_capi_init_str(JSContext *ctx);   /* kind ops STR/BYTES/BYTEARRAY/MEMORYVIEW, codec registry glue */
/* New immortal interned str mirror for an ASCII/UTF-8 name (used by A's
   identifier table). */
static PyObject *qp_capi_str_immortal(JSContext *ctx, const char *utf8);
/* Live size of a BYTEARRAY mirror (A's _PyQp_GetLiveSize/_SetLiveSize
   dispatch on kind BYTEARRAY to these; set = PyByteArray_Resize semantics,
   errors are left pending). */
static Py_ssize_t qp_capi_bytearray_live_size(PyObject *op);
static void qp_capi_bytearray_set_live_size(PyObject *op, Py_ssize_t n);
/* Buffer slot bridges (B installs them):
   - forward: QPTypeSlots.bf of a C type whose tp_as_buffer->bf_getbuffer is
     set (QP side consuming a C exporter: bytes(array.array), memoryview(x)).
     The Py_buffer lives in a block found again through view->format.
   - reverse: tp_as_buffer of bound builtin / Python types whose QP
     bf_getbuffer is set (C code calling PyBytes_Type.tp_as_buffer->...):
     PyObject_GetBuffer / PyBuffer_Release semantics for J-backed objects. */
static int qp_capi_bf_getbuffer_fwd(JSContext *ctx, JSValueConst obj, QPBuffer *view, int flags);
static void qp_capi_bf_releasebuffer_fwd(JSContext *ctx, QPBuffer *view);
static int qp_capi_bf_getbuffer_rev(PyObject *exporter, Py_buffer *view, int flags);
static void qp_capi_bf_releasebuffer_rev(PyObject *exporter, Py_buffer *view);

/* ========================================================================= */
/* (E1) CONTAINERS, ERRORS: python/capi_tuple.c, python/capi_dict.c,          */
/*      python/capi_errors.c                                                  */
/* ========================================================================= */

static int qp_capi_init_containers(JSContext *ctx); /* kind ops TUPLE/LIST/DICT/SET/SLICE/CELL/WEAKREF, PyCapsule_Type */
static int qp_capi_init_errors(JSContext *ctx);     /* PyExc_* pointers, kind ops EXCEPTION */
/* Exception classes: static PyTypeObject per QP_EXCEPTIONS entry, exported
   through the PyExc_<name> pointers (PyAPI_DATA(PyObject *)). */
/* List mirrors (kind LIST, _Py_QP_LIVE_SIZE_FLAG), used by A's exported
   _PyQp_GetLiveSize / _PyQp_SetLiveSize / _PyQp_ListItems:
   - qp_capi_list_size: current length of the JS array;
   - qp_capi_list_set_size: Py_SET_SIZE (truncates; growing pads None);
     errors stay pending;
   - qp_capi_list_items: PyObject* array snapshot of the items (borrowed,
     pinned per index; valid until the next mutation), NULL + exception. */
static Py_ssize_t qp_capi_list_size(PyObject *op);
static void qp_capi_list_set_size(PyObject *op, Py_ssize_t n);
static PyObject **qp_capi_list_items(PyObject *op);
/* tp_alloc of a C type whose solid base is tuple (IsoCalendarDate, ...):
   a C-primary tuple mirror of `type` with n NULL items; its JS value, an
   instance of the subtype, is built at the first crossing. */
static PyObject *qp_capi_tuple_alloc_type(PyTypeObject *type, Py_ssize_t n);

/* ========================================================================= */
/* (E2) ARGUMENTS, RUNTIME SERVICES: python/capi_getargs.c,                   */
/*      python/capi_runtime.c                                                 */
/* ========================================================================= */

static int qp_capi_init_runtime(JSContext *ctx);    /* thread/interpreter state, kind ops CODE */
/* (M5 Tier B) python/capi_stable.c: the remaining stable-ABI exports; runs
   after qp_capi_init_runtime (Py_UTF8Mode from the configuration). */
static int qp_capi_init_stable(JSContext *ctx);

/* Thread state (M3: single thread; M5 makes the GIL real).  The main
   PyThreadState is allocated by qp_capi_init_runtime and current except
   between PyEval_SaveThread() and PyEval_RestoreThread(). */
struct QPCapiThreadState;   /* private: PyThreadState + _PyThreadStateImpl fields used by modules */

/* ========================================================================= */
/* TRACEMALLOC: python/capi_tracemalloc.c                                     */
/* ========================================================================= */

/* Set while tracemalloc traces: PyMem_Raw* (and PyMem_*, which share the
   raw allocator) go through the qp_capi_tm_raw_* functions, which also
   trace the block.  The runtime's js_malloc_rt() and co. (hence the
   Python objects and PyObject_Malloc) are traced through
   JSRuntime.py_tm_tracing.  Read with relaxed atomics (PyMem_Raw* may be
   called without the GIL). */
static int qp_capi_tm_tracing;
static void *qp_capi_tm_raw_alloc(size_t nelem, size_t elsize, BOOL zero);
static void *qp_capi_tm_raw_realloc(void *ptr, size_t new_size);
static void qp_capi_tm_raw_free(void *ptr);
/* _PyMem_DumpTraceback (the debug hooks on the allocators) */
static void qp_capi_tm_dump_traceback(int fd, const void *ptr);
/* tracemalloc.stop() when the C-API context goes away */
static void qp_capi_tm_context_free(JSContext *ctx);

#endif /* QP_CAPI_H */
