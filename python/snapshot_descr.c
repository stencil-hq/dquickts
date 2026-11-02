/*
 * dquickts Python heap snapshot: builtin functions and descriptors.
 *
 * Part file of python/snapshot.c (included there, same translation unit).
 * Payloads of JS_CLASS_PY_CFUNCTION, PY_DESCR, PY_CLASSMETHOD,
 * PY_STATICMETHOD, PY_PROPERTY and PY_SUPER.
 *
 * Every payload starts with a presence byte: properties, classmethods,
 * staticmethods (and subclasses) may be allocated without a payload (it is
 * created on first use), and the finalizers of the other classes accept a
 * NULL payload too.
 *
 * Static C data referenced by these payloads (QPMethodDef / QPGetSetDef
 * entries, wrapped slot functions) goes through the native registry
 * (qp_snap_put_def: u8 0 + native).  Definitions of C extensions (C API
 * registry) are written as (owner, attribute name), u8 1: the reader
 * takes the definition of that attribute once the owning C module ran
 * again (python/capi_snapshot.c); other unregistered definitions make the
 * write fail with a TypeError.  The slotdefs entry of a wrapper descriptor is written
 * as its index in qp_slotdefs (the build signature pins the table), which
 * lets the reader validate it before the descriptor dereferences it.
 */

/* --- builtin_function_or_method ------------------------------------------ */

static int qp_snap_descr_write_cfunction(SnapWriter *w, JSObject *p)
{
    QPCFunctionObject *f = p->u.opaque;

    bc_put_u8(&w->bc, f != NULL);
    if (!f)
        return 0;
    if (qp_snap_put_def(w, f->ml, QP_SNAP_DEF_METHOD) ||
        snap_put_value(w, f->self) ||
        snap_put_value(w, f->module))
        return -1;
    return 0;
}

static int qp_snap_descr_read_cfunction(SnapReader *r, JSObject *p)
{
    QPCFunctionObject *f;
    uint8_t present;
    BOOL is_ref;

    if (bc_get_u8(&r->bc, &present))
        return -1;
    if (!present)
        return 0;
    f = js_mallocz(r->ctx, sizeof(*f));
    if (!f)
        return -1;
    f->self = QP_NULL;
    f->module = QP_NULL;
    p->u.opaque = f;
    snap_payload_ready(r);
    if (qp_snap_get_def(r, (const void **)&f->ml, QP_SNAP_DEF_METHOD, &is_ref))
        return -1;
    if (!f->ml && !is_ref)
        return snap_error(r, "invalid builtin function definition");
    if (snap_get_value(r, &f->self) ||
        snap_get_value(r, &f->module))
        return -1;
    return 0;
}

/* --- method/classmethod/getset/member/wrapper descriptors, method-wrapper - */

static int qp_snap_descr_write_descr(SnapWriter *w, JSObject *p)
{
    QPDescr *d = p->u.opaque;

    bc_put_u8(&w->bc, d != NULL);
    if (!d)
        return 0;
    bc_put_u8(&w->bc, d->kind);
    if (snap_put_value(w, d->objclass) ||
        bc_put_atom(&w->bc, d->name) ||
        snap_put_value(w, d->self))
        return -1;
    switch (d->kind) {
    case QP_DESCR_METHOD:
    case QP_DESCR_CLASSMETHOD:
        return qp_snap_put_def(w, d->u.method, QP_SNAP_DEF_METHOD);
    case QP_DESCR_GETSET:
        return qp_snap_put_def(w, d->u.getset, QP_SNAP_DEF_GETSET);
    case QP_DESCR_MEMBER:
        /* The private atom is the key of the hidden property holding the
           slot value in instances of non-PY_OBJECT classes: the atom table
           of the snapshot keeps its identity, so those properties stay
           reachable through the restored descriptor.  A PyMemberDef-backed
           member of a C-extension type has an accessor definition. */
        bc_put_leb128(&w->bc, d->u.member.slot);
        bc_put_u8(&w->bc, d->u.member.flags);
        if (bc_put_atom(&w->bc, d->u.member.private_atom))
            return -1;
        bc_put_u8(&w->bc, d->u.member.getset != NULL);
        if (!d->u.member.getset)
            return 0;
        return qp_snap_put_def(w, d->u.member.getset, QP_SNAP_DEF_GETSET);
    case QP_DESCR_WRAPPER:
    case QP_DESCR_METHOD_WRAPPER:
        /* a method-wrapper copies the slot and wrapped function of its
           wrapper_descriptor and keeps its receiver in d->self */
        if (!d->u.wrapper.slot || d->u.wrapper.slot < qp_slotdefs ||
            d->u.wrapper.slot >= qp_slotdefs + QP_NSLOTDEFS)
            return qp_snap_reject(w, p, "slot wrapper outside the slot table");
        bc_put_u16(&w->bc, (uint16_t)(d->u.wrapper.slot - qp_slotdefs));
        return qp_snap_put_def(w, d->u.wrapper.wrapped, QP_SNAP_DEF_WRAPPED);
    default:
        return qp_snap_reject(w, p, "unknown descriptor kind");
    }
}

static int qp_snap_descr_read_descr(SnapReader *r, JSObject *p)
{
    QPDescr *d;
    uint8_t present, kind, flags, has_getset;
    uint16_t slot_index;
    uint32_t slot;
    BOOL is_ref;

    if (bc_get_u8(&r->bc, &present))
        return -1;
    if (!present)
        return 0;
    if (bc_get_u8(&r->bc, &kind))
        return -1;
    if (kind > QP_DESCR_METHOD_WRAPPER)
        return snap_error(r, "invalid descriptor kind");
    d = js_mallocz(r->ctx, sizeof(*d));
    if (!d)
        return -1;
    /* kind is set before the payload is published so that the finalizer
       frees the member private atom (JS_ATOM_NULL until read) correctly */
    d->kind = kind;
    d->objclass = QP_NULL;
    d->self = QP_NULL;
    d->name = JS_ATOM_NULL;
    p->u.opaque = d;
    snap_payload_ready(r);
    if (snap_get_value(r, &d->objclass) ||
        bc_get_atom(&r->bc, &d->name) ||
        snap_get_value(r, &d->self))
        return -1;
    switch (kind) {
    case QP_DESCR_METHOD:
    case QP_DESCR_CLASSMETHOD:
        if (qp_snap_get_def(r, (const void **)&d->u.method, QP_SNAP_DEF_METHOD, &is_ref))
            return -1;
        if (!d->u.method && !is_ref)
            return snap_error(r, "invalid method descriptor");
        break;
    case QP_DESCR_GETSET:
        if (qp_snap_get_def(r, (const void **)&d->u.getset, QP_SNAP_DEF_GETSET, &is_ref))
            return -1;
        if (!d->u.getset && !is_ref)
            return snap_error(r, "invalid getset descriptor");
        break;
    case QP_DESCR_MEMBER:
        if (bc_get_leb128(&r->bc, &slot) ||
            bc_get_u8(&r->bc, &flags))
            return -1;
        if (slot > UINT16_MAX)
            return snap_error(r, "invalid member descriptor slot");
        d->u.member.slot = slot;
        d->u.member.flags = flags;
        if (bc_get_atom(&r->bc, &d->u.member.private_atom) ||
            bc_get_u8(&r->bc, &has_getset))
            return -1;
        if (has_getset) {
            if (qp_snap_get_def(r, (const void **)&d->u.member.getset, QP_SNAP_DEF_GETSET,
                                &is_ref))
                return -1;
            if (!d->u.member.getset && !is_ref)
                return snap_error(r, "invalid member descriptor");
        }
        break;
    case QP_DESCR_WRAPPER:
    case QP_DESCR_METHOD_WRAPPER:
        if (bc_get_u16(&r->bc, &slot_index))
            return -1;
        if (slot_index >= QP_NSLOTDEFS)
            return snap_error(r, "invalid wrapper descriptor slot");
        d->u.wrapper.slot = &qp_slotdefs[slot_index];
        if (qp_snap_get_def(r, (const void **)&d->u.wrapper.wrapped, QP_SNAP_DEF_WRAPPED,
                            &is_ref))
            return -1;
        break;
    }
    return 0;
}

/* --- classmethod / staticmethod ------------------------------------------- */

static int qp_snap_descr_write_funcwrap(SnapWriter *w, JSObject *p)
{
    QPFuncWrap *fw = p->u.opaque;

    bc_put_u8(&w->bc, fw != NULL);
    if (!fw)
        return 0;
    if (snap_put_value(w, fw->callable) ||
        snap_put_value(w, fw->dict))
        return -1;
    return 0;
}

static int qp_snap_descr_read_funcwrap(SnapReader *r, JSObject *p)
{
    QPFuncWrap *fw;
    uint8_t present;

    if (bc_get_u8(&r->bc, &present))
        return -1;
    if (!present)
        return 0;
    fw = js_mallocz(r->ctx, sizeof(*fw));
    if (!fw)
        return -1;
    fw->callable = QP_NULL;
    fw->dict = QP_NULL;
    p->u.opaque = fw;
    snap_payload_ready(r);
    if (snap_get_value(r, &fw->callable) ||
        snap_get_value(r, &fw->dict))
        return -1;
    return 0;
}

/* --- property --------------------------------------------------------------- */

static int qp_snap_descr_write_property(SnapWriter *w, JSObject *p)
{
    QPProperty *pp = p->u.opaque;

    bc_put_u8(&w->bc, pp != NULL);
    if (!pp)
        return 0;
    if (snap_put_value(w, pp->fget) ||
        snap_put_value(w, pp->fset) ||
        snap_put_value(w, pp->fdel) ||
        snap_put_value(w, pp->doc) ||
        snap_put_value(w, pp->name))
        return -1;
    bc_put_sleb128(&w->bc, pp->getter_doc);
    return 0;
}

static int qp_snap_descr_read_property(SnapReader *r, JSObject *p)
{
    QPProperty *pp;
    uint8_t present;
    int32_t getter_doc;

    if (bc_get_u8(&r->bc, &present))
        return -1;
    if (!present)
        return 0;
    pp = js_mallocz(r->ctx, sizeof(*pp));
    if (!pp)
        return -1;
    pp->fget = QP_NULL;
    pp->fset = QP_NULL;
    pp->fdel = QP_NULL;
    pp->doc = QP_NULL;
    pp->name = QP_NULL;
    p->u.opaque = pp;
    snap_payload_ready(r);
    if (snap_get_value(r, &pp->fget) ||
        snap_get_value(r, &pp->fset) ||
        snap_get_value(r, &pp->fdel) ||
        snap_get_value(r, &pp->doc) ||
        snap_get_value(r, &pp->name) ||
        bc_get_sleb128(&r->bc, &getter_doc))
        return -1;
    pp->getter_doc = getter_doc;
    return 0;
}

/* --- super ------------------------------------------------------------------ */

/* A super object only holds its resolved __thisclass__ / __self__ /
   __self_class__ (the zero-argument form reads the frame when it is
   created, not later), so it is not tied to the frame that made it. */
static int qp_snap_descr_write_super(SnapWriter *w, JSObject *p)
{
    QPSuper *su = p->u.opaque;

    bc_put_u8(&w->bc, su != NULL);
    if (!su)
        return 0;
    if (snap_put_value(w, su->type) ||
        snap_put_value(w, su->obj) ||
        snap_put_value(w, su->obj_type))
        return -1;
    return 0;
}

static int qp_snap_descr_read_super(SnapReader *r, JSObject *p)
{
    QPSuper *su;
    uint8_t present;

    if (bc_get_u8(&r->bc, &present))
        return -1;
    if (!present)
        return 0;
    su = js_mallocz(r->ctx, sizeof(*su));
    if (!su)
        return -1;
    su->type = QP_NULL;
    su->obj = QP_NULL;
    su->obj_type = QP_NULL;
    p->u.opaque = su;
    snap_payload_ready(r);
    if (snap_get_value(r, &su->type) ||
        snap_get_value(r, &su->obj) ||
        snap_get_value(r, &su->obj_type))
        return -1;
    return 0;
}

/* --- dispatch --------------------------------------------------------------- */

static int qp_snap_write_descr(SnapWriter *w, JSObject *p)
{
    switch (p->class_id) {
    case JS_CLASS_PY_CFUNCTION:
        return qp_snap_descr_write_cfunction(w, p);
    case JS_CLASS_PY_DESCR:
        return qp_snap_descr_write_descr(w, p);
    case JS_CLASS_PY_CLASSMETHOD:
    case JS_CLASS_PY_STATICMETHOD:
        return qp_snap_descr_write_funcwrap(w, p);
    case JS_CLASS_PY_PROPERTY:
        return qp_snap_descr_write_property(w, p);
    case JS_CLASS_PY_SUPER:
        return qp_snap_descr_write_super(w, p);
    default:
        return 1;
    }
}

static int qp_snap_read_descr(SnapReader *r, JSObject *p)
{
    switch (p->class_id) {
    case JS_CLASS_PY_CFUNCTION:
        return qp_snap_descr_read_cfunction(r, p);
    case JS_CLASS_PY_DESCR:
        return qp_snap_descr_read_descr(r, p);
    case JS_CLASS_PY_CLASSMETHOD:
    case JS_CLASS_PY_STATICMETHOD:
        return qp_snap_descr_read_funcwrap(r, p);
    case JS_CLASS_PY_PROPERTY:
        return qp_snap_descr_read_property(r, p);
    case JS_CLASS_PY_SUPER:
        return qp_snap_descr_read_super(r, p);
    default:
        return 1;
    }
}
