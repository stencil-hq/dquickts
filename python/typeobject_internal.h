/*
 * Private interface between python/typeobject.c and python/descrobject.c
 * (unit h).  Both files are #included into quickjs.c; this header only
 * declares static functions and private structures shared by them.
 *
 * Derived from CPython Objects/typeobject.c and Objects/descrobject.c
 * (PSF-2.0).
 */
#ifndef QP_TYPEOBJECT_INTERNAL_H
#define QP_TYPEOBJECT_INTERNAL_H

/* ---- slotdefs (typeobject.c) ------------------------------------------- */

/* Slot wrapper implementation (CPython wrapperfunc / wrapperfunc_kwds):
   `self` is the bound object, args exclude it; `wrapped` is the C slot
   function of the wrapper descriptor.  kwnames is QP_NULL unless the
   slotdef has QP_SLOTDEF_KEYWORDS (the caller rejects keywords with
   "wrapper %s() takes no keyword arguments" otherwise). */
typedef JSValue QPWrapperFunc(JSContext *ctx, JSValueConst self, JSValueConst *args,
                              int nargs, JSValueConst kwnames, void *wrapped);

#define QP_SLOTDEF_KEYWORDS 0x01   /* PyWrapperFlag_KEYWORDS */

struct QPSlotDef {
    const char *name;           /* dunder name, e.g. "__add__" */
    int atom_id;                /* QP_ATOM_* of name */
    int offset;                 /* offsetof(QPTypeSlots, <slot>) */
    void *function;             /* slot_* dispatcher installed on heap types */
    QPWrapperFunc *wrapper;     /* wraps the C slot for the wrapper_descriptor */
    const char *doc;            /* "__add__($self, value, /)\n--\n\nReturn self+value." */
    int flags;                  /* QP_SLOTDEF_* */
};

/* ---- QPType private extension (typeobject.c) --------------------------- */

/* Every QPType is allocated as a QPTypeExt: raw pointers to the base
   QPTypes (kept consistent in both directions by the finalizers, so that
   GC cycle collection can free bases and subclasses in any order). */
typedef struct QPTypeExt {
    QPType t;
    QPType **bases_raw;
    int nbases_raw;
    JSValue doc;                /* builtin types: cached __doc__ str or QP_NULL */
} QPTypeExt;

/* New str: CPython tp_name, used by "'%.100s' object ..." messages
   (builtins: spec name such as "int" or "module.Name"; heap types:
   __name__). */
static JSValue qp_type_tp_name(JSContext *ctx, QPType *t);
/* New str: __qualname__ of t (heap: ht_qualname; builtins: short name). */
static JSValue qp_type_qualname(JSContext *ctx, QPType *t);
/* New str: __module__ of t ("builtins" for undotted builtin names). */
static JSValue qp_type_module(JSContext *ctx, QPType *t);
/* Short name (part of tp_name after the last '.'), new str. */
static JSValue qp_type_short_name(JSContext *ctx, QPType *t);

/* object.__new__ / object.__init__ (typeobject.c), compared by the
   excess-argument checks and by tp_new_wrapper. */
static JSValue qp_object_new(JSContext *ctx, QPType *subtype, JSValueConst *args,
                             size_t nargsf, JSValueConst kwnames);
static int qp_object_init(JSContext *ctx, JSValueConst self, JSValueConst *args,
                          size_t nargsf, JSValueConst kwnames);

/* ---- descriptor constructors (descrobject.c) --------------------------- */

/* member_descriptor for a slot: name owned by the caller (duplicated). */
static JSValue qp_descr_new_member(JSContext *ctx, QPType *t, JSAtom name, uint16_t slot,
                                   uint8_t flags);
/* wrapper_descriptor (slot wrapper) for slotdef `sd` wrapping `wrapped`. */
static JSValue qp_descr_new_wrapper(JSContext *ctx, QPType *t, const QPSlotDef *sd,
                                    void *wrapped);
/* classmethod_descriptor for a QP_METH_CLASS entry. */
static JSValue qp_descr_new_classmethod(JSContext *ctx, QPType *t, const QPMethodDef *ml);
/* QPDescr payload of a PY_DESCR object, NULL if v is not one. */
static QPDescr *qp_descr_get(JSValueConst v);
/* Type of a descriptor object: QP_DESCR_* kind or -1. */
static int qp_descr_kind(JSValueConst v);
/* Callable of a classmethod/staticmethod object (borrowed), QP_NULL if not
   one. */
static JSValue qp_funcwrap_callable(JSValueConst v);

/* Doc helpers (CPython _PyType_GetDocFromInternalDoc /
   _PyType_GetTextSignatureFromInternalDoc): `name` is the object name the
   signature block must start with; None when absent. */
static JSValue qp_doc_from_internal(JSContext *ctx, const char *name, const char *doc);
static JSValue qp_text_signature_from_internal(JSContext *ctx, const char *name,
                                               const char *doc);

/* init of the descriptor types, builtin_function_or_method, property,
   classmethod, staticmethod (called by qp_init_typeobject). */
static int qp_init_descr_types(JSContext *ctx);

/* Real isinstance/issubclass without hooks (abstract.c). */
static int qp_object_real_isinstance(JSContext *ctx, JSValueConst inst, JSValueConst cls);
static int qp_object_real_issubclass(JSContext *ctx, JSValueConst derived, JSValueConst cls);

#endif /* QP_TYPEOBJECT_INTERNAL_H */
