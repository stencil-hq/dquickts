/*
 * dquickts Python heap snapshot, part `buf` (included by python/snapshot.c):
 * payloads of bytesobject.c / bytearrayobject.c / memoryobject.c,
 * exceptions.c, traceback.c and moduleobject.c.
 *
 *   JS_CLASS_PY_BYTES       QPBytesObject (kind QP_BK_BYTES, inline data)
 *   JS_CLASS_PY_BYTEARRAY   QPBytesObject (QP_BK_BYTEARRAY), QPMemoryView
 *                           (QP_BK_MEMORYVIEW), QPManagedBuffer (QP_BK_MBUF)
 *   JS_CLASS_PY_EXCEPTION   QPExcObject
 *   JS_CLASS_PY_TRACEBACK   QPTraceback
 *   JS_CLASS_PY_MODULE      QPModule
 *
 * Every payload starts with a u8: the QP_BK_* kind for the bytes classes,
 * 1 for the others; 0 means "no payload" (u.opaque == NULL), restored as
 * such.
 *
 * Payload layouts after that byte:
 *   bytes        i64 len, len raw bytes                       (hash: -1)
 *   bytearray    i64 len, i64 alloc, len raw bytes            (exports: recounted)
 *   managed buf  u8 released, [value master.obj if !released] (exports: recounted)
 *   memoryview   u8 released, value mbuf, i64 len, i64 itemsize, u8 readonly,
 *                leb ndim, bytes format, ndim x (i64 shape, i64 stride),
 *                [i64 byte offset of buf from the exporter's base if !released]
 *                (hash: -1, exports: 0, flags: recomputed by qp_mv_init_flags)
 *   exception    value args, context, cause, traceback, u8 suppress_context,
 *                i64 written, i64 start, i64 end, QP_EXC_NFIELDS x value f[i]
 *   traceback    value next, func, frame, self_hint, locals,
 *                sleb lasti, sleb lineno
 *   module       value dict, u8 builtin
 *
 * Buffer pointers are never written.  QPBuffer.buf / len / readonly /
 * itemsize / format of a managed buffer are re-derived from its restored
 * exporter exactly as the exporter's bf_getbuffer computes them
 * (qp_snap_buf_exporter), and a memoryview's buf is that base plus the
 * stored byte offset.  This runs in an EARLY deferred function: only then
 * are the exporters' bodies (bytes inline data, bytearray storage, typed
 * array storage) guaranteed to exist, and it must precede the normal
 * deferred functions because dict/set re-hashing may hash a read-only
 * memoryview key (which reads mv->buf).
 *
 * Export counts are recounted instead of copied, so that they describe the
 * restored heap exactly (holders that were not part of the snapshot no
 * longer pin a bytearray):
 *   - bytearray.exports starts at 0; each unreleased managed buffer over it
 *     adds one (what qp_bytearray_getbuffer did when the buffer was made);
 *   - managed buffer exports start at 0; each unreleased memoryview
 *     registered with it adds one (qp_mv_alloc);
 *   - memoryview.exports is 0: the only long-lived QPBuffer holder is a
 *     managed buffer, and a managed buffer never exports a memoryview
 *     (qp_memoryview_from_object copies the source view's registration,
 *     and qp_snap_buf_exporter rejects memoryview exporters).
 * A managed buffer left without registered views after restore (all its
 * live views were outside the snapshot) is released in a normal deferred
 * function, as memory_release / mbuf_release would have done.
 */

/* ------------------------------------------------------------------------ */
/* raw byte strings                                                          */
/* ------------------------------------------------------------------------ */

static int qp_snap_buf_get_raw(SnapReader *r, uint8_t *dst, int64_t len)
{
    while (len > 0) {
        uint32_t n = len > 0x40000000 ? 0x40000000 : (uint32_t)len;
        if (bc_get_buf(&r->bc, dst, n))
            return -1;
        dst += n;
        len -= n;
    }
    return 0;
}

/* Length prefix of a raw byte string: must fit in the remaining input. */
static int qp_snap_buf_get_len(SnapReader *r, int64_t *plen)
{
    int64_t len;
    if (qp_snap_get_i64(r, &len))
        return -1;
    if (len < 0 || len > (int64_t)(r->bc.buf_end - r->bc.ptr))
        return snap_error(r, "invalid bytes length");
    *plen = len;
    return 0;
}

/* ------------------------------------------------------------------------ */
/* buffer exporters                                                          */
/* ------------------------------------------------------------------------ */

/* The buffer `obj` exports, computed like its bf_getbuffer (qp_buffer_get:
   qp_bytes_getbuffer, qp_bytearray_getbuffer, qp_memoryview_getbuffer on a
   typed array, the ArrayBuffer branch) but without taking a reference or
   counting an export.  Only fills buf/len/readonly/itemsize/format.
   -1 for exporters whose memory cannot be re-derived from the heap image
   (C-extension buffers, memoryviews, missing payloads). */
static int qp_snap_buf_exporter(JSValueConst obj, QPBuffer *view)
{
    JSObject *p;
    int k = qp_bk_kind(obj);

    if (k == QP_BK_BYTES) {
        QPBytesObject *b = QP_BYTES_OBJ(obj);
        view->buf = b->data;
        view->len = b->len;
        view->readonly = 1;
        view->itemsize = 1;
        view->format = "B";
        return 0;
    }
    if (k == QP_BK_BYTEARRAY) {
        QPBytesObject *b = QP_BYTES_OBJ(obj);
        view->buf = b->data ? b->data : (uint8_t *)qp_bytes_empty_data;
        view->len = b->len;
        view->readonly = 0;
        view->itemsize = 1;
        view->format = "B";
        return 0;
    }
    if (k != 0 || JS_VALUE_GET_TAG(obj) != JS_TAG_OBJECT)
        return -1;
    p = JS_VALUE_GET_OBJ(obj);
    if (!qp_bm_is_typed_array_class(p->class_id) && p->class_id != JS_CLASS_ARRAY_BUFFER &&
        p->class_id != JS_CLASS_SHARED_ARRAY_BUFFER)
        return -1;
    qp_bm_js_buffer_info(p, &view->buf, &view->len, &view->readonly, &view->itemsize,
                         &view->format);
    if (!view->buf)
        view->buf = (uint8_t *)qp_bytes_empty_data;
    return 0;
}

/* Whether every byte the view can address (its elements, and for
   contiguous views the len bytes copied by tobytes & co.) lies within
   [0, avail) of the exporter's buffer when buf = base + offset. */
static BOOL qp_snap_buf_mv_fits(const QPMemoryView *mv, int64_t offset, int64_t avail)
{
    int64_t lo = 0, hi = 0;
    int i;

    if (offset < 0 || offset > avail || mv->len < 0 || mv->itemsize < 1)
        return FALSE;
    if ((mv->flags & (QP_MV_C | QP_MV_FORTRAN)) && mv->len > avail - offset)
        return FALSE;
    for (i = 0; i < mv->ndim; i++) {
        if (mv->shape[i] < 0)
            return FALSE;
        if (mv->shape[i] == 0)
            return TRUE;        /* no element */
    }
    for (i = 0; i < mv->ndim; i++) {
        int64_t n = mv->shape[i] - 1, s = mv->strides[i];
        if (n == 0 || s == 0)
            continue;
        if (s == INT64_MIN || (s < 0 ? -s : s) > avail / n)
            return FALSE;
        if (s < 0)
            lo += n * s;
        else
            hi += n * s;
        if (lo < -avail || hi > avail)
            return FALSE;
    }
    return offset + lo >= 0 && mv->itemsize <= avail && offset + hi <= avail - mv->itemsize;
}

/* ------------------------------------------------------------------------ */
/* bytes / bytearray / memoryview / managed buffer: writer                   */
/* ------------------------------------------------------------------------ */

static int qp_snap_buf_write_mbuf(SnapWriter *w, JSObject *p, QPManagedBuffer *mb)
{
    QPBuffer info;

    bc_put_u8(&w->bc, mb->released);
    if (mb->released)
        return 0;
    if (qp_snap_buf_exporter(mb->master.obj, &info) < 0)
        return qp_snap_reject(w, p, "managed buffer over an exporter whose memory cannot be "
                              "re-derived (only bytes, bytearray and JS ArrayBuffer / typed "
                              "arrays are supported)");
    /* A JS ArrayBuffer can be detached or reallocated while exported (no
       export lock on the JS side): the views would then be dangling. */
    if (info.buf != mb->master.buf)
        return qp_snap_reject(w, p, "the exporter's memory moved since the buffer was acquired");
    return snap_put_value(w, mb->master.obj);
}

static int qp_snap_buf_write_mv(SnapWriter *w, JSObject *p, QPMemoryView *mv)
{
    BOOL released = (mv->flags & QP_MV_RELEASED) != 0;
    int64_t offset = 0;
    int i;

    if (!released) {
        QPManagedBuffer *mb;
        QPBuffer info;
        if (qp_bk_kind(mv->mbuf) != QP_BK_MBUF)
            return qp_snap_reject(w, p, "memoryview without a managed buffer");
        mb = QP_MBUF_OBJ(mv->mbuf);
        if (mb->released)
            return qp_snap_reject(w, p, "memoryview over a released managed buffer");
        if (qp_snap_buf_exporter(mb->master.obj, &info) < 0)
            return qp_snap_reject(w, p, "memoryview over an exporter whose memory cannot be "
                                  "re-derived");
        if (info.buf != mb->master.buf)
            return qp_snap_reject(w, p, "the exporter's memory moved since the view was made");
        offset = (int64_t)(mv->buf - info.buf);
        if (!qp_snap_buf_mv_fits(mv, offset, info.len))
            return qp_snap_reject(w, p, "memoryview extends past its exporter's buffer "
                                  "(exporter resized)");
    }
    bc_put_u8(&w->bc, released);
    if (snap_put_value(w, mv->mbuf))
        return -1;
    qp_snap_put_i64(w, mv->len);
    qp_snap_put_i64(w, mv->itemsize);
    bc_put_u8(&w->bc, mv->readonly != 0);
    bc_put_leb128(&w->bc, mv->ndim);
    snap_put_bytes(&w->bc, mv->format, strnlen(mv->format, QP_MV_FMT_MAX));
    for (i = 0; i < mv->ndim; i++) {
        qp_snap_put_i64(w, mv->shape[i]);
        qp_snap_put_i64(w, mv->strides[i]);
    }
    if (!released)
        qp_snap_put_i64(w, offset);
    return 0;
}

static int qp_snap_buf_write_bk(SnapWriter *w, JSObject *p)
{
    void *op = p->u.opaque;
    int kind = op ? *(uint8_t *)op : 0;

    if (kind != 0 && (kind == QP_BK_BYTES) != (p->class_id == JS_CLASS_PY_BYTES))
        return qp_snap_reject(w, p, "inconsistent bytes payload");
    bc_put_u8(&w->bc, kind);
    switch (kind) {
    case 0:
        return 0;
    case QP_BK_BYTES: {
        QPBytesObject *b = op;
        qp_snap_put_i64(w, b->len);
        dbuf_put(&w->bc.dbuf, b->data, b->len);
        return 0;
    }
    case QP_BK_BYTEARRAY: {
        QPBytesObject *b = op;
        qp_snap_put_i64(w, b->len);
        qp_snap_put_i64(w, b->alloc);
        if (b->len > 0)
            dbuf_put(&w->bc.dbuf, b->data, b->len);
        return 0;
    }
    case QP_BK_MEMORYVIEW:
        return qp_snap_buf_write_mv(w, p, op);
    case QP_BK_MBUF:
        return qp_snap_buf_write_mbuf(w, p, op);
    default:
        return qp_snap_reject(w, p, "unknown bytes payload kind");
    }
}

/* ------------------------------------------------------------------------ */
/* bytes / bytearray / memoryview / managed buffer: reader                   */
/* ------------------------------------------------------------------------ */

/* Early deferred: re-acquire the exporter's buffer (see the file comment). */
static int qp_snap_buf_mbuf_fix(JSContext *ctx, JSObject *p)
{
    QPManagedBuffer *mb = p->u.opaque;
    QPBuffer info;

    if (mb->released)
        return 0;
    if (qp_snap_buf_exporter(mb->master.obj, &info) < 0) {
        JS_ThrowTypeError(ctx, "snapshot: invalid managed buffer exporter");
        return -1;
    }
    mb->master.buf = info.buf;
    mb->master.len = info.len;
    mb->master.readonly = info.readonly;
    mb->master.itemsize = info.itemsize;
    mb->master.format = info.format;
    if (qp_bk_kind(mb->master.obj) == QP_BK_BYTEARRAY)
        QP_BYTES_OBJ(mb->master.obj)->exports++;
    return 0;
}

/* Normal deferred (after every early fn has counted the views): a managed
   buffer nobody is registered with is released (mbuf_release). */
static int qp_snap_buf_mbuf_settle(JSContext *ctx, JSObject *p)
{
    QPManagedBuffer *mb = p->u.opaque;

    if (!mb->released && mb->exports == 0)
        qp_mbuf_release(ctx, mb);
    return 0;
}

/* Early deferred for unreleased views.  Until it runs the view stays
   flagged QP_MV_RELEASED (so a failed read finalizes it without touching
   its managed buffer's count) and `exports` holds the stored byte offset of
   buf from the exporter's base. */
static int qp_snap_buf_mv_fix(JSContext *ctx, JSObject *p)
{
    QPMemoryView *mv = p->u.opaque;
    QPManagedBuffer *mb;
    QPBuffer info;
    int64_t offset = mv->exports;

    if (qp_bk_kind(mv->mbuf) != QP_BK_MBUF)
        goto fail;
    mb = QP_MBUF_OBJ(mv->mbuf);
    /* derive the base from the exporter itself: the managed buffer's own
       fix-up may not have run yet */
    if (mb->released || qp_snap_buf_exporter(mb->master.obj, &info) < 0)
        goto fail;
    if (!mv->readonly && info.readonly)
        goto fail;
    mv->flags = 0;
    qp_mv_init_flags(mv);
    if (!qp_snap_buf_mv_fits(mv, offset, info.len)) {
        mv->flags = QP_MV_RELEASED;
        goto fail;
    }
    mv->buf = info.buf + offset;
    mv->exports = 0;
    mb->exports++;
    return 0;
fail:
    JS_ThrowTypeError(ctx, "snapshot: invalid memoryview");
    return -1;
}

static int qp_snap_buf_read_mv(SnapReader *r, JSObject *p)
{
    QPMemoryView *mv;
    uint8_t released, readonly;
    uint32_t ndim, fmtlen;
    int64_t v;
    int i;

    mv = js_mallocz(r->ctx, sizeof(*mv));
    if (!mv)
        return -1;
    mv->kind = QP_BK_MEMORYVIEW;
    mv->flags = QP_MV_RELEASED;
    mv->mbuf = JS_UNDEFINED;
    mv->hash = -1;
    mv->buf = (uint8_t *)qp_bytes_empty_data;
    qp_mv_set_dims_inline(mv);
    p->u.opaque = mv;
    snap_payload_ready(r);

    if (bc_get_u8(&r->bc, &released) || snap_get_value(r, &mv->mbuf))
        return -1;
    if (qp_snap_get_i64(r, &v))
        return -1;
    mv->len = v;
    if (qp_snap_get_i64(r, &v))
        return -1;
    mv->itemsize = v;
    if (bc_get_u8(&r->bc, &readonly) || bc_get_leb128(&r->bc, &ndim) ||
        bc_get_leb128(&r->bc, &fmtlen))
        return -1;
    if (released > 1 || readonly > 1 || ndim > QP_MV_MAX_NDIM || fmtlen >= QP_MV_FMT_MAX ||
        mv->len < 0 || mv->itemsize < 1)
        return snap_error(r, "invalid memoryview");
    mv->readonly = readonly;
    if (bc_get_buf(&r->bc, (uint8_t *)mv->format, fmtlen))
        return -1;
    if (ndim > 1) {
        mv->shape = js_malloc(r->ctx, sizeof(Py_ssize_t) * 2 * ndim);
        if (!mv->shape) {
            qp_mv_set_dims_inline(mv);
            return -1;
        }
        mv->strides = mv->shape + ndim;
    }
    mv->ndim = ndim;
    for (i = 0; i < (int)ndim; i++) {
        if (qp_snap_get_i64(r, &v))
            return -1;
        mv->shape[i] = v;
        if (qp_snap_get_i64(r, &v))
            return -1;
        mv->strides[i] = v;
    }
    if (released) {
        qp_mv_init_flags(mv);   /* keeps QP_MV_RELEASED */
        return 0;
    }
    if (qp_snap_get_i64(r, &v))
        return -1;
    if (v < 0)
        return snap_error(r, "invalid memoryview");
    mv->exports = v;    /* stashed offset, see qp_snap_buf_mv_fix */
    return qp_snap_defer_early(r, p, qp_snap_buf_mv_fix);
}

static int qp_snap_buf_read_mbuf(SnapReader *r, JSObject *p)
{
    QPManagedBuffer *mb;
    uint8_t released;
    JSValue obj;

    mb = js_mallocz(r->ctx, sizeof(*mb));
    if (!mb)
        return -1;
    mb->kind = QP_BK_MBUF;
    mb->released = 1;
    mb->master.obj = JS_UNDEFINED;
    mb->master.buf = (uint8_t *)qp_bytes_empty_data;
    mb->master.itemsize = 1;
    mb->master.readonly = 1;
    mb->master.format = "B";
    p->u.opaque = mb;
    snap_payload_ready(r);

    if (bc_get_u8(&r->bc, &released))
        return -1;
    if (released > 1)
        return snap_error(r, "invalid managed buffer");
    if (released)
        return 0;
    if (snap_get_value(r, &obj))
        return -1;
    mb->master.obj = obj;
    mb->released = 0;
    if (qp_snap_defer_early(r, p, qp_snap_buf_mbuf_fix) ||
        qp_snap_defer(r, p, qp_snap_buf_mbuf_settle))
        return -1;
    return 0;
}

static int qp_snap_buf_read_bk(SnapReader *r, JSObject *p)
{
    uint8_t kind;
    int64_t len, alloc;
    QPBytesObject *b;

    if (bc_get_u8(&r->bc, &kind))
        return -1;
    if (kind == 0) {
        snap_payload_ready(r);
        return 0;
    }
    if (kind > QP_BK_MBUF || (kind == QP_BK_BYTES) != (p->class_id == JS_CLASS_PY_BYTES))
        return snap_error(r, "invalid bytes payload kind");
    switch (kind) {
    case QP_BK_BYTES:
        if (qp_snap_buf_get_len(r, &len))
            return -1;
        b = js_malloc(r->ctx, sizeof(QPBytesObject) + len + 1);
        if (!b)
            return -1;
        b->kind = QP_BK_BYTES;
        b->len = len;
        b->data = b->inline_data;
        b->hash = -1;
        b->alloc = 0;
        b->exports = 0;
        b->inline_data[len] = '\0';
        p->u.opaque = b;
        snap_payload_ready(r);
        return qp_snap_buf_get_raw(r, b->inline_data, len);
    case QP_BK_BYTEARRAY:
        if (qp_snap_buf_get_len(r, &len) || qp_snap_get_i64(r, &alloc))
            return -1;
        /* alloc can exceed 2*len (take_bytes keeps the allocation) */
        if (alloc < len || alloc >= (int64_t)PY_SSIZE_T_MAX)
            return snap_error(r, "invalid bytearray");
        b = js_mallocz(r->ctx, sizeof(QPBytesObject));
        if (!b)
            return -1;
        b->kind = QP_BK_BYTEARRAY;
        b->hash = -1;
        p->u.opaque = b;
        snap_payload_ready(r);
        if (alloc == 0)
            return 0;
        b->data = js_malloc(r->ctx, alloc + 1);
        if (!b->data)
            return -1;
        b->alloc = alloc;
        b->data[len] = '\0';
        if (qp_snap_buf_get_raw(r, b->data, len))
            return -1;
        b->len = len;
        return 0;
    case QP_BK_MEMORYVIEW:
        return qp_snap_buf_read_mv(r, p);
    default:
        return qp_snap_buf_read_mbuf(r, p);
    }
}

/* ------------------------------------------------------------------------ */
/* exceptions                                                                */
/* ------------------------------------------------------------------------ */

static int qp_snap_buf_write_exc(SnapWriter *w, JSObject *p)
{
    QPExcObject *e = p->u.opaque;
    int i;

    bc_put_u8(&w->bc, e != NULL);
    if (!e)
        return 0;
    if (snap_put_value(w, e->args) || snap_put_value(w, e->context) ||
        snap_put_value(w, e->cause) || snap_put_value(w, e->traceback))
        return -1;
    bc_put_u8(&w->bc, e->suppress_context);
    qp_snap_put_i64(w, e->written);
    qp_snap_put_i64(w, e->start);
    qp_snap_put_i64(w, e->end);
    for (i = 0; i < QP_EXC_NFIELDS; i++) {
        if (snap_put_value(w, e->f[i]))
            return -1;
    }
    return 0;
}

static int qp_snap_buf_read_exc(SnapReader *r, JSObject *p)
{
    QPExcObject *e;
    uint8_t present, suppress;
    int64_t v;
    int i;

    if (bc_get_u8(&r->bc, &present))
        return -1;
    if (present > 1)
        return snap_error(r, "invalid exception payload");
    if (!present) {
        snap_payload_ready(r);
        return 0;
    }
    e = js_mallocz(r->ctx, sizeof(*e));
    if (!e)
        return -1;
    e->args = QP_NULL;
    e->context = QP_NULL;
    e->cause = QP_NULL;
    e->traceback = QP_NULL;
    for (i = 0; i < QP_EXC_NFIELDS; i++)
        e->f[i] = QP_NULL;
    p->u.opaque = e;
    snap_payload_ready(r);

    if (snap_get_value(r, &e->args) || snap_get_value(r, &e->context) ||
        snap_get_value(r, &e->cause) || snap_get_value(r, &e->traceback) ||
        bc_get_u8(&r->bc, &suppress))
        return -1;
    e->suppress_context = suppress != 0;
    if (qp_snap_get_i64(r, &v))
        return -1;
    e->written = v;
    if (qp_snap_get_i64(r, &v))
        return -1;
    e->start = v;
    if (qp_snap_get_i64(r, &v))
        return -1;
    e->end = v;
    for (i = 0; i < QP_EXC_NFIELDS; i++) {
        if (snap_get_value(r, &e->f[i]))
            return -1;
    }
    return 0;
}

/* ------------------------------------------------------------------------ */
/* tracebacks                                                                */
/* ------------------------------------------------------------------------ */

/* lasti is a pc into the bytecode of `func`, which the snapshot restores
   with the same layout, so it stays valid; `frame` (the lazily created
   frame facade) is an ordinary PY_FRAME value whose own payload writer
   decides whether it can be saved. */
static int qp_snap_buf_write_tb(SnapWriter *w, JSObject *p)
{
    QPTraceback *tb = p->u.opaque;

    bc_put_u8(&w->bc, tb != NULL);
    if (!tb)
        return 0;
    if (snap_put_value(w, tb->next) || snap_put_value(w, tb->func) ||
        snap_put_value(w, tb->frame) || snap_put_value(w, tb->self_hint) ||
        snap_put_value(w, tb->locals))
        return -1;
    bc_put_sleb128(&w->bc, tb->lasti);
    bc_put_sleb128(&w->bc, tb->lineno);
    return 0;
}

static int qp_snap_buf_read_tb(SnapReader *r, JSObject *p)
{
    QPTraceback *tb;
    uint8_t present;
    int32_t v;

    if (bc_get_u8(&r->bc, &present))
        return -1;
    if (present > 1)
        return snap_error(r, "invalid traceback payload");
    if (!present) {
        snap_payload_ready(r);
        return 0;
    }
    tb = js_mallocz(r->ctx, sizeof(*tb));
    if (!tb)
        return -1;
    tb->next = JS_UNDEFINED;
    tb->func = QP_NULL;
    tb->frame = QP_NULL;
    tb->self_hint = QP_NULL;
    tb->locals = QP_NULL;
    p->u.opaque = tb;
    snap_payload_ready(r);

    if (snap_get_value(r, &tb->next) || snap_get_value(r, &tb->func) ||
        snap_get_value(r, &tb->frame) || snap_get_value(r, &tb->self_hint) ||
        snap_get_value(r, &tb->locals))
        return -1;
    if (bc_get_sleb128(&r->bc, &v))
        return -1;
    tb->lasti = v;
    if (bc_get_sleb128(&r->bc, &v))
        return -1;
    tb->lineno = v;
    return 0;
}

/* ------------------------------------------------------------------------ */
/* modules                                                                   */
/* ------------------------------------------------------------------------ */

/* QPModule holds no module def pointer and no C module state (md_state):
   builtin modules keep their state in md_dict or in QPState, so the dict and
   the repr flag are the whole payload. */
static int qp_snap_buf_write_module(SnapWriter *w, JSObject *p)
{
    QPModule *m = p->u.opaque;

    bc_put_u8(&w->bc, m != NULL);
    if (!m)
        return 0;
    if (snap_put_value(w, m->dict))
        return -1;
    bc_put_u8(&w->bc, m->builtin);
    return 0;
}

static int qp_snap_buf_read_module(SnapReader *r, JSObject *p)
{
    QPModule *m;
    uint8_t present, builtin;

    if (bc_get_u8(&r->bc, &present))
        return -1;
    if (present > 1)
        return snap_error(r, "invalid module payload");
    if (!present) {
        snap_payload_ready(r);
        return 0;
    }
    m = js_mallocz(r->ctx, sizeof(*m));
    if (!m)
        return -1;
    m->dict = JS_UNDEFINED;
    p->u.opaque = m;
    snap_payload_ready(r);

    if (snap_get_value(r, &m->dict) || bc_get_u8(&r->bc, &builtin))
        return -1;
    if (builtin > 1)
        return snap_error(r, "invalid module payload");
    m->builtin = builtin;
    return 0;
}

/* ------------------------------------------------------------------------ */
/* dispatch                                                                  */
/* ------------------------------------------------------------------------ */

static int qp_snap_write_buf(SnapWriter *w, JSObject *p)
{
    switch (p->class_id) {
    case JS_CLASS_PY_BYTES:
    case JS_CLASS_PY_BYTEARRAY:
        return qp_snap_buf_write_bk(w, p);
    case JS_CLASS_PY_EXCEPTION:
        return qp_snap_buf_write_exc(w, p);
    case JS_CLASS_PY_TRACEBACK:
        return qp_snap_buf_write_tb(w, p);
    case JS_CLASS_PY_MODULE:
        return qp_snap_buf_write_module(w, p);
    default:
        return 1;
    }
}

static int qp_snap_read_buf(SnapReader *r, JSObject *p)
{
    switch (p->class_id) {
    case JS_CLASS_PY_BYTES:
    case JS_CLASS_PY_BYTEARRAY:
        return qp_snap_buf_read_bk(r, p);
    case JS_CLASS_PY_EXCEPTION:
        return qp_snap_buf_read_exc(r, p);
    case JS_CLASS_PY_TRACEBACK:
        return qp_snap_buf_read_tb(r, p);
    case JS_CLASS_PY_MODULE:
        return qp_snap_buf_read_module(r, p);
    default:
        return 1;
    }
}
