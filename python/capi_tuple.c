/*
 * dquickts C API (D5, unit E1): tuple, list, slice and cell objects.
 *
 * Semantics ported from CPython 3.15.2 Objects/tupleobject.c,
 * Objects/listobject.c, Objects/sliceobject.c and Objects/cellobject.c
 * (PSF-2.0).  Part of the quickjs.c translation unit (included by
 * python/quickpy.c after python/capi.h); the exported Py* / _Py* functions
 * are the C API, everything else is static.
 *
 * Layouts (capi-design.md §1):
 * - TUPLE: CPython's PyTupleObject.  A mirror of a JS tuple materializes
 *   ob_item[] with owned references when it is created (nested tuples are
 *   filled iteratively, without C recursion); a tuple created by C code is
 *   C-primary and gets its JS value at its first crossing (to_js).
 * - LIST: always the JS array; PyListObject is a handle with a live size
 *   (_Py_QP_LIVE_SIZE_FLAG) and borrowed items pinned per index.
 * - SLICE: start/stop/step materialized (owned references).
 * - CELL: the JS cell; the content is pinned (role CELL) when borrowed.
 */

/* Internal exports of CPython's internal headers (pycore_tuple.h,
   pycore_list.h): the compat shims declare them for the modules; they are
   declared here for this translation unit. */
PyAPI_FUNC(PyObject *) _PyTuple_FromArraySteal(PyObject *const *src, Py_ssize_t n);
PyAPI_FUNC(PyObject *) _PyTuple_FromPair(PyObject *a, PyObject *b);
PyAPI_FUNC(PyObject *) _PyTuple_FromPairSteal(PyObject *a, PyObject *b);
PyAPI_FUNC(int) _PyList_AppendTakeRefListResize(PyListObject *self, PyObject *newitem);

/* Private helpers of the M2/M3 units used by unit E1 (same translation
   unit; repeated here so the files also check on their own). */
/* listobject.c (unit e) */
static int qp_lst_fast(JSContext *ctx, JSObject *p);
static int qp_lst_mfast(JSContext *ctx, JSObject *p);
static int qp_lst_reserve(JSContext *ctx, JSObject *p, Py_ssize_t n);
static int qp_lst_push(JSContext *ctx, JSObject *p, JSValue v);
static int qp_lst_clear(JSContext *ctx, JSObject *p);
static JSValue qp_lst_slice(JSContext *ctx, JSObject *p, Py_ssize_t ilow, Py_ssize_t ihigh);
static int qp_lst_ass_slice(JSContext *ctx, JSValueConst self, Py_ssize_t ilow,
                            Py_ssize_t ihigh, JSValueConst v);
/* sliceobject.c (unit e) */
static int qp_slice_get_long_indices(JSContext *ctx, JSValueConst slice, JSValueConst length,
                                     JSValue *pstart, JSValue *pstop, JSValue *pstep);
/* errors.c (unit i) */
static void qp_err_clear(JSContext *ctx);
static void qp_err_normalize_pending(JSContext *ctx);
static JSValue qp_err_create_exception(JSContext *ctx, QPType *type, JSValueConst value);
static void qp_exc_write_unraisable_msg(JSContext *ctx, JSValueConst exc,
                                        const char *err_msg, JSValueConst obj);
static int qp_err_system_exit_code(JSContext *ctx, JSValueConst exc);
static int qp_file_write_str(JSContext *ctx, JSValueConst file, JSValueConst str);
static int qp_file_write_cstr(JSContext *ctx, JSValueConst file, const char *s);
static void qp_file_flush(JSContext *ctx, JSValueConst file);
/* exceptions.c (unit i) */
static JSValue qp_exc_context(JSValueConst exc);
static JSValue qp_exc_cause(JSValueConst exc);
static void qp_exc_set_context(JSContext *ctx, JSValueConst exc, JSValue context);
static void qp_exc_set_cause(JSContext *ctx, JSValueConst exc, JSValue cause);
static JSValue qp_exc_traceback(JSValueConst exc);
static void qp_exc_set_traceback_raw(JSContext *ctx, JSValueConst exc, JSValue tb);
static JSValue qp_exc_args(JSValueConst exc);
/* traceback.c (unit i) */
static JSValue qp_tb_alloc(JSContext *ctx, JSValueConst next, JSValueConst func,
                           JSValueConst frame, int lasti, int lineno);
static JSValue qp_tb_source_line_stripped(JSContext *ctx, JSValueConst filename, int lineno);
/* sysmodule.c (unit k) */
static JSValue qp_sys_get(JSContext *ctx, JSAtom name);
/* mod_misc.c (M3: _warnings, _signal) */
static int qp_warnings_warn(JSContext *ctx, QPType *category, JSValueConst message,
                            Py_ssize_t stack_level, JSValueConst source);
static int qp_warnings_warn_explicit(JSContext *ctx, QPType *category, JSValueConst message,
                                     JSValueConst filename, int lineno, JSValueConst module,
                                     JSValueConst registry, JSValueConst source);
static int qp_signal_check(JSContext *ctx);
static int qp_signal_trip(JSContext *ctx, int signum);
/* mod_weakref.c (M3: _weakref) */
static JSValue qp_weakref_new(JSContext *ctx, JSValueConst obj, JSValueConst callback);
static JSValue qp_weakref_new_proxy(JSContext *ctx, JSValueConst obj, JSValueConst callback);
static BOOL qp_weakref_check(JSContext *ctx, JSValueConst v);

/* ========================================================================= */
/* Shared helpers of unit E1                                                  */
/* ========================================================================= */

/* Borrowed JS value of a J-backed container mirror; SystemError when the
   JS object was collected while the mirror was alive (ORPHAN). */
static JSValue qp_capi_e1_jv(JSContext *ctx, PyObject *op)
{
    if (QP_CAPI_HAS_HEAD(op) && (QP_CAPI_HEAD(op)->flags & QP_CAPI_F_ORPHAN)) {
        qp_raise_system_error(ctx, "%s object used after its Python object was freed",
                              Py_TYPE(op)->tp_name);
        return JS_EXCEPTION;
    }
    return qp_capi_value(ctx, op);
}

/* Identity of two JS values as Python objects (`is` for the values a
   mirror can be bound to). */
static BOOL qp_capi_e1_same(JSValueConst a, JSValueConst b)
{
    if (JS_VALUE_GET_TAG(a) != JS_VALUE_GET_TAG(b))
        return FALSE;
    switch (JS_VALUE_GET_TAG(a)) {
    case JS_TAG_INT:
    case JS_TAG_BOOL:
        return JS_VALUE_GET_INT(a) == JS_VALUE_GET_INT(b);
    case JS_TAG_SHORT_BIG_INT:
        return JS_VALUE_GET_SHORT_BIG_INT(a) == JS_VALUE_GET_SHORT_BIG_INT(b);
    case JS_TAG_FLOAT64: {
        double x = JS_VALUE_GET_FLOAT64(a), y = JS_VALUE_GET_FLOAT64(b);
        return memcmp(&x, &y, sizeof(double)) == 0;
    }
    case JS_TAG_UNDEFINED:
    case JS_TAG_NULL:
        return TRUE;
    default:
        return JS_VALUE_HAS_REF_COUNT(a) && JS_VALUE_GET_PTR(a) == JS_VALUE_GET_PTR(b);
    }
}

/* Pending exception put aside while running code that may fail or clear
   it, then restored exactly (the uncatchable flag of interrupts too);
   errors raised meanwhile are discarded. */
typedef struct QPCapiErrSave {
    JSValue exc;
    BOOL uncatchable;
} QPCapiErrSave;

static void qp_capi_err_save(JSContext *ctx, QPCapiErrSave *s)
{
    s->uncatchable = ctx->rt->current_exception_is_uncatchable;
    s->exc = JS_HasException(ctx) ? JS_GetException(ctx) : QP_NULL;
}

static void qp_capi_err_restore(JSContext *ctx, QPCapiErrSave *s)
{
    if (QP_IS_NULL(s->exc)) {
        qp_err_clear(ctx);
        return;
    }
    JS_Throw(ctx, s->exc);
    if (s->uncatchable)
        JS_SetUncatchableException(ctx, TRUE);
}

/* Borrowed JS value of an optional argument: NULL -> QP_NULL. */
static JSValue qp_capi_e1_opt(JSContext *ctx, PyObject *op)
{
    return op ? qp_capi_value(ctx, op) : QP_NULL;
}

/* ========================================================================= */
/* TUPLE kind                                                                 */
/* ========================================================================= */

#define QP_CAPI_TUPLE_HDR (offsetof(PyTupleObject, ob_item))

static size_t qp_capi_tuple_extra(Py_ssize_t n)
{
    return QP_CAPI_TUPLE_HDR - sizeof(PyObject) + (size_t)n * sizeof(PyObject *);
}

/* Mirrors created during one materialization: work[0..count) in creation
   order; work[done..count) still have to be filled.  Nested tuple mirrors
   created while filling (depth > 0) only append themselves, so a deeply
   nested tuple is filled by the loop of the outermost init. */
#define QP_CAPI_TUPLE_FILL_SMALL 32
static PyObject *qp_capi_tuple_fill_small[QP_CAPI_TUPLE_FILL_SMALL];
static struct {
    PyObject **work;            /* qp_capi_tuple_fill_small, or a heap block while needed */
    size_t count, size, done;
    int depth;
} qp_capi_tuple_fill = { qp_capi_tuple_fill_small, 0, QP_CAPI_TUPLE_FILL_SMALL, 0, 0 };

/* A non-tuple kind init that reads the items of the tuple mirrors it
   creates (code: co_varnames, ...) may run inside a tuple fill (depth > 0),
   where new tuple mirrors would only be queued for the outer loop and have
   NULL items: suspend makes them fill immediately (the re-entry path of
   qp_capi_tuple_init: their work entries start at the current count),
   resume restores the outer loop's state. */
typedef struct QPCapiTupleFillSave {
    size_t done;
    int depth;
} QPCapiTupleFillSave;

static void qp_capi_tuple_fill_suspend(QPCapiTupleFillSave *s)
{
    s->done = qp_capi_tuple_fill.done;
    s->depth = qp_capi_tuple_fill.depth;
    qp_capi_tuple_fill.depth = 0;
}

static void qp_capi_tuple_fill_resume(const QPCapiTupleFillSave *s)
{
    qp_capi_tuple_fill.done = s->done;
    qp_capi_tuple_fill.depth = s->depth;
}

static int qp_capi_tuple_push(JSContext *ctx, PyObject *op)
{
    if (qp_capi_tuple_fill.count == qp_capi_tuple_fill.size) {
        size_t nsize = qp_capi_tuple_fill.size * 2;
        PyObject **nw = js_malloc_rt(ctx->rt, nsize * sizeof(PyObject *));
        if (!nw) {
            JS_ThrowOutOfMemory(ctx);
            return -1;
        }
        memcpy(nw, qp_capi_tuple_fill.work, qp_capi_tuple_fill.count * sizeof(PyObject *));
        if (qp_capi_tuple_fill.work != qp_capi_tuple_fill_small)
            js_free_rt(ctx->rt, qp_capi_tuple_fill.work);
        qp_capi_tuple_fill.work = nw;
        qp_capi_tuple_fill.size = nsize;
    }
    qp_capi_tuple_fill.work[qp_capi_tuple_fill.count++] = op;
    return 0;
}

/* Back to the static work array once no materialization is running. */
static void qp_capi_tuple_fill_reset(JSRuntime *rt)
{
    if (qp_capi_tuple_fill.count == 0 && qp_capi_tuple_fill.work != qp_capi_tuple_fill_small) {
        js_free_rt(rt, qp_capi_tuple_fill.work);
        qp_capi_tuple_fill.work = qp_capi_tuple_fill_small;
        qp_capi_tuple_fill.size = QP_CAPI_TUPLE_FILL_SMALL;
    }
}

/* Fill ob_item[] of a J-backed tuple mirror from its JS tuple. */
static int qp_capi_tuple_fill_one(JSContext *ctx, PyObject *op)
{
    PyTupleObject *t = (PyTupleObject *)op;
    JSValueConst v = QP_CAPI_HEAD(op)->v;
    Py_ssize_t i, n = Py_SIZE(op);

    for (i = 0; i < n; i++) {
        JSValueConst iv = QP_TUPLE_ITEMS(v)[i];
        PyObject *item;
        if (QP_IS_NULL(iv)) {
            t->ob_item[i] = NULL;   /* a slot C code has not set yet */
            continue;
        }
        if (qp_tuple_check_exact(ctx, iv)) {
            /* a nested tuple only queues itself (filled by the outer loop) */
            item = qp_capi_new_ref(ctx, iv);
        } else {
            /* another kind may read the items of the tuples its init
               makes (code: co_varnames): they are filled by a loop of
               their own, which must not disturb this one */
            size_t done = qp_capi_tuple_fill.done;
            int depth = qp_capi_tuple_fill.depth;
            qp_capi_tuple_fill.depth = 0;
            item = qp_capi_new_ref(ctx, iv);
            qp_capi_tuple_fill.depth = depth;
            qp_capi_tuple_fill.done = done;
        }
        if (!item)
            return -1;
        t->ob_item[i] = item;
    }
    return 0;
}

static size_t qp_capi_tuple_size(JSContext *ctx, JSValueConst v)
{
    return qp_capi_tuple_extra(QP_TUPLE_SIZE(v));
}

static int qp_capi_tuple_init(JSContext *ctx, PyObject *op, JSValueConst v)
{
    PyTupleObject *t = (PyTupleObject *)op;
    Py_ssize_t n = QP_TUPLE_SIZE(v);
    size_t i, start;
    int ret = 0;

    Py_SET_SIZE(t, n);
    t->ob_hash = QP_TUPLE(v)->hash;
    if (n == 0)
        return 0;
    if (qp_capi_tuple_fill.depth > 0)
        return qp_capi_tuple_push(ctx, op);    /* filled by the outer loop */
    start = qp_capi_tuple_fill.count;          /* 0 unless re-entered from another kind */
    qp_capi_tuple_fill.done = start;
    if (qp_capi_tuple_push(ctx, op) < 0)
        return -1;
    qp_capi_tuple_fill.depth++;
    while (qp_capi_tuple_fill.done < qp_capi_tuple_fill.count) {
        PyObject *w = qp_capi_tuple_fill.work[qp_capi_tuple_fill.done++];
        if (qp_capi_tuple_fill_one(ctx, w) < 0) {
            ret = -1;
            break;
        }
    }
    qp_capi_tuple_fill.depth--;
    /* only completely filled mirrors may persist (survive refcount 0):
       on failure the partial ones are released with the outer mirror */
    if (ret == 0) {
        for (i = start; i < qp_capi_tuple_fill.count; i++)
            QP_CAPI_HEAD(qp_capi_tuple_fill.work[i])->flags |= QP_CAPI_F_PERSIST;
    }
    qp_capi_tuple_fill.count = start;
    qp_capi_tuple_fill.done = start;
    qp_capi_tuple_fill_reset(ctx->rt);
    return ret;
}

/* C-primary tuple at its first crossing: the JS tuple of its items. */
static JSValue qp_capi_tuple_to_js(JSContext *ctx, PyObject *op)
{
    PyTupleObject *t = (PyTupleObject *)op;
    Py_ssize_t i, n = Py_SIZE(op);
    QPType *sub = NULL;
    JSValue *vals, res;

    if (Py_TYPE(op) != &PyTuple_Type) {
        sub = qp_capi_type_qp(Py_TYPE(op));
        if (!sub)
            return JS_EXCEPTION;
    } else if (n == 0) {
        return JS_DupValue(ctx, QP_STATE(ctx)->empty_tuple);
    }
    vals = js_malloc(ctx, sizeof(JSValue) * (size_t)(n ? n : 1));
    if (!vals)
        return JS_EXCEPTION;
    for (i = 0; i < n; i++) {
        if (!t->ob_item[i]) {
            /* A tuple may cross before C filled it: CPython's dicts and
               lists store the pointer, and the owner fills the items
               afterwards with PyTuple_SetItem (Modules/_scproxy.c), which
               updates the bound JS tuple (qp_capi_tuple_store_js).  The
               empty slot stays NULL in the JS tuple (repr "<NULL>"), like
               store_js of NULL. */
            vals[i] = QP_NULL;
            continue;
        }
        vals[i] = qp_capi_to_js(ctx, t->ob_item[i]);
        if (JS_IsException(vals[i]))
            goto fail;
    }
    if (sub) {
        res = qp_tup_alloc(ctx, sub, n);
        if (JS_IsException(res)) {
            while (i-- > 0)
                JS_FreeValue(ctx, vals[i]);
        } else if (n) {
            memcpy(QP_TUPLE_ITEMS(res), vals, sizeof(JSValue) * (size_t)n);
        }
    } else {
        res = qp_tuple_new_steal(ctx, n, vals);
    }
    js_free(ctx, vals);
    if (!JS_IsException(res)) {
        if (t->ob_hash != -1)
            QP_TUPLE(res)->hash = t->ob_hash;
        if (QP_CAPI_HAS_HEAD(op))
            QP_CAPI_HEAD(op)->flags |= QP_CAPI_F_PERSIST;
    }
    return res;
 fail:
    while (i-- > 0)
        JS_FreeValue(ctx, vals[i]);
    js_free(ctx, vals);
    return JS_EXCEPTION;
}

/* Re-sync ob_item[] with the JS tuple (struct sequences fill their JS
   tuple after the mirror exists, B's PyStructSequence_SetItem). */
static void qp_capi_tuple_refresh(JSContext *ctx, PyObject *op)
{
    PyTupleObject *t = (PyTupleObject *)op;
    QPCapiErrSave saved;
    BOOL saving = FALSE;
    QPCapiHead *h;
    Py_ssize_t i, n;

    if (!QP_CAPI_HAS_HEAD(op))
        return;
    h = QP_CAPI_HEAD(op);
    if (QP_IS_NULL(h->v) || (h->flags & QP_CAPI_F_ORPHAN))
        return;
    n = Py_SIZE(op);
    if (QP_TUPLE_SIZE(h->v) < n)
        n = QP_TUPLE_SIZE(h->v);
    for (i = 0; i < n; i++) {
        JSValueConst jv = QP_TUPLE_ITEMS(h->v)[i];
        PyObject *old = t->ob_item[i], *item;
        if (QP_IS_NULL(jv)) {
            t->ob_item[i] = NULL;
            Py_XDECREF(old);
            continue;
        }
        if (old) {
            if (QP_CAPI_HAS_HEAD(old)) {
                QPCapiHead *oh = QP_CAPI_HEAD(old);
                if (!QP_IS_NULL(oh->v) && !(oh->flags & QP_CAPI_F_ORPHAN) &&
                    qp_capi_e1_same(oh->v, jv))
                    continue;
            } else {
                /* static objects: no allocation, cannot fail */
                JSValue sv = qp_capi_value(ctx, old);
                if (!JS_IsException(sv) && qp_capi_e1_same(sv, jv))
                    continue;
            }
        }
        if (!saving) {
            qp_capi_err_save(ctx, &saved);
            saving = TRUE;
        }
        item = qp_capi_new_ref(ctx, jv);
        if (!item)
            continue;   /* out of memory: keep the previous item */
        t->ob_item[i] = item;
        Py_XDECREF(old);
    }
    if (saving)
        qp_capi_err_restore(ctx, &saved);
    t->ob_hash = QP_TUPLE(h->v)->hash;
}

static int qp_capi_tuple_traverse(PyObject *op, visitproc visit, void *arg)
{
    PyTupleObject *t = (PyTupleObject *)op;
    Py_ssize_t i;
    for (i = Py_SIZE(op); --i >= 0; )
        Py_VISIT(t->ob_item[i]);
    return 0;
}

static void qp_capi_tuple_clear(JSRuntime *rt, PyObject *op)
{
    PyTupleObject *t = (PyTupleObject *)op;
    Py_ssize_t i;
    for (i = Py_SIZE(op); --i >= 0; )
        Py_CLEAR(t->ob_item[i]);
}

/* ---- tuple API ---------------------------------------------------------- */

static PyObject *qp_capi_tuple_empty(void)
{
    return Py_NewRef(_PyQp_Singleton(QP_CAPI_SINGLETON_TUPLE_EMPTY));
}

/* New C-primary tuple with NULL items (n > 0). */
static PyObject *qp_capi_tuple_alloc_type(PyTypeObject *type, Py_ssize_t n)
{
    PyTupleObject *t;
    size_t hdr = QP_CAPI_TUPLE_HDR;
    if ((size_t)type->tp_basicsize > hdr)
        hdr = (size_t)type->tp_basicsize;
    if ((size_t)n > (PY_SSIZE_T_MAX - hdr) / sizeof(PyObject *))
        return PyErr_NoMemory();
    t = (PyTupleObject *)qp_capi_alloc(type, hdr + (size_t)n * sizeof(PyObject *),
                                       QP_CAPI_K_TUPLE);
    if (!t)
        return NULL;
    Py_SET_SIZE(t, n);
    t->ob_hash = -1;
    return (PyObject *)t;
}

static PyObject *qp_capi_tuple_alloc(Py_ssize_t n)
{
    return qp_capi_tuple_alloc_type(&PyTuple_Type, n);
}

PyObject *PyTuple_New(Py_ssize_t size)
{
    if (size == 0)
        return qp_capi_tuple_empty();
    if (size < 0) {
        PyErr_BadInternalCall();
        return NULL;
    }
    return qp_capi_tuple_alloc(size);
}

Py_ssize_t PyTuple_Size(PyObject *op)
{
    if (!PyTuple_Check(op)) {
        PyErr_BadInternalCall();
        return -1;
    }
    return Py_SIZE(op);
}

PyObject *PyTuple_GetItem(PyObject *op, Py_ssize_t i)
{
    if (!PyTuple_Check(op)) {
        PyErr_BadInternalCall();
        return NULL;
    }
    if (i < 0 || i >= Py_SIZE(op)) {
        PyErr_SetString(PyExc_IndexError, "tuple index out of range");
        return NULL;
    }
    return ((PyTupleObject *)op)->ob_item[i];
}

/* A bound tuple is about to be mutated in place: when only its mirror
   references the JS tuple, the JS value is dropped (the tuple becomes
   C-primary again); returns 1 if the JS tuple must be updated as well. */
static int qp_capi_tuple_detach(PyObject *op)
{
    QPCapiHead *h = QP_CAPI_HEAD(op);
    JSObject *p;
    int own;

    if (QP_IS_NULL(h->v) || (h->flags & QP_CAPI_F_ORPHAN))
        return 0;
    p = JS_VALUE_GET_OBJ(h->v);
    own = (h->flags & QP_CAPI_F_OWNS_V) ? 1 : 0;
    if (js_rc(p)->ref_count <= own) {
        qp_capi_unbind(op);
        h->flags &= ~QP_CAPI_F_PERSIST;
        return 0;
    }
    return 1;
}

/* _PyObject_IsUniquelyReferenced of a bound object (pycore_object.h): the
   caller may now write the items of a tuple in place */
void _PyQp_Unshare(PyObject *op)
{
    if (PyTuple_CheckExact(op) && QP_CAPI_HAS_HEAD(op))
        (void)qp_capi_tuple_detach(op);
}

/* Store the JS value of `value` in slot i of the JS tuple of op. */
static void qp_capi_tuple_store_js(JSContext *ctx, PyObject *op, Py_ssize_t i, PyObject *value)
{
    QPCapiHead *h = QP_CAPI_HEAD(op);
    QPTuple *qt = QP_TUPLE(h->v);
    JSValue nv = value ? qp_capi_to_js(ctx, value) : QP_NULL;
    JSValue old;

    if (JS_IsException(nv))
        return;     /* error stays pending */
    old = qt->items[i];
    qt->items[i] = nv;
    qt->hash = -1;
    JS_FreeValue(ctx, old);
}

/* PyTuple_SET_ITEM on a bound tuple: steals value, does not release the
   previous item (CPython's macro contract). */
void _PyQp_TupleSetItem(PyObject *op, Py_ssize_t index, PyObject *value)
{
    PyTupleObject *t = (PyTupleObject *)op;
    t->ob_item[index] = value;
    t->ob_hash = -1;
    if (_PyQp_HasFlag(op, _Py_QP_BOUND_FLAG) && qp_capi_tuple_detach(op))
        qp_capi_tuple_store_js(qp_capi.ctx, op, index, value);
}

int PyTuple_SetItem(PyObject *op, Py_ssize_t i, PyObject *newitem)
{
    PyTupleObject *t = (PyTupleObject *)op;
    PyObject *old;

    if (!PyTuple_Check(op) || Py_REFCNT(op) != 1) {
        Py_XDECREF(newitem);
        PyErr_BadInternalCall();
        return -1;
    }
    if (i < 0 || i >= Py_SIZE(op)) {
        Py_XDECREF(newitem);
        PyErr_SetString(PyExc_IndexError, "tuple assignment index out of range");
        return -1;
    }
    old = t->ob_item[i];
    t->ob_item[i] = newitem;
    t->ob_hash = -1;
    if (_PyQp_HasFlag(op, _Py_QP_BOUND_FLAG) && qp_capi_tuple_detach(op))
        qp_capi_tuple_store_js(qp_capi.ctx, op, i, newitem);
    Py_XDECREF(old);
    return 0;
}

PyObject *PyTuple_GetSlice(PyObject *op, Py_ssize_t ilow, Py_ssize_t ihigh)
{
    PyTupleObject *a = (PyTupleObject *)op;
    PyObject *np;
    Py_ssize_t i, len;

    if (op == NULL || !PyTuple_Check(op)) {
        PyErr_BadInternalCall();
        return NULL;
    }
    if (ilow < 0)
        ilow = 0;
    if (ihigh > Py_SIZE(a))
        ihigh = Py_SIZE(a);
    if (ihigh < ilow)
        ihigh = ilow;
    if (ilow == 0 && ihigh == Py_SIZE(a) && PyTuple_CheckExact(op))
        return Py_NewRef(op);
    len = ihigh - ilow;
    if (len == 0)
        return qp_capi_tuple_empty();
    np = qp_capi_tuple_alloc(len);
    if (!np)
        return NULL;
    for (i = 0; i < len; i++)
        ((PyTupleObject *)np)->ob_item[i] = Py_NewRef(a->ob_item[ilow + i]);
    return np;
}

PyObject *PyTuple_Pack(Py_ssize_t n, ...)
{
    PyObject *res;
    Py_ssize_t i;
    va_list ap;

    if (n == 0)
        return qp_capi_tuple_empty();
    res = PyTuple_New(n);
    if (!res)
        return NULL;
    va_start(ap, n);
    for (i = 0; i < n; i++)
        ((PyTupleObject *)res)->ob_item[i] = Py_NewRef(va_arg(ap, PyObject *));
    va_end(ap);
    return res;
}

PyObject *PyTuple_FromArray(PyObject *const *src, Py_ssize_t n)
{
    PyObject *res;
    Py_ssize_t i;

    if (n == 0)
        return qp_capi_tuple_empty();
    res = PyTuple_New(n);
    if (!res)
        return NULL;
    for (i = 0; i < n; i++)
        ((PyTupleObject *)res)->ob_item[i] = Py_NewRef(src[i]);
    return res;
}

PyObject *_PyTuple_FromArraySteal(PyObject *const *src, Py_ssize_t n)
{
    PyObject *res;
    Py_ssize_t i;

    if (n == 0)
        return qp_capi_tuple_empty();
    res = PyTuple_New(n);
    if (!res) {
        for (i = 0; i < n; i++)
            Py_DECREF(src[i]);
        return NULL;
    }
    for (i = 0; i < n; i++)
        ((PyTupleObject *)res)->ob_item[i] = src[i];
    return res;
}

PyObject *_PyTuple_FromPair(PyObject *a, PyObject *b)
{
    PyObject *res = qp_capi_tuple_alloc(2);
    if (!res)
        return NULL;
    ((PyTupleObject *)res)->ob_item[0] = Py_NewRef(a);
    ((PyTupleObject *)res)->ob_item[1] = Py_NewRef(b);
    return res;
}

PyObject *_PyTuple_FromPairSteal(PyObject *a, PyObject *b)
{
    PyObject *res = qp_capi_tuple_alloc(2);
    if (!res) {
        Py_DECREF(a);
        Py_DECREF(b);
        return NULL;
    }
    ((PyTupleObject *)res)->ob_item[0] = a;
    ((PyTupleObject *)res)->ob_item[1] = b;
    return res;
}

/* _testinternalcapi.tuple_from_pair / tuple_from_pair_steal
   (mod_testinternalcapi.c): the tuple made by _PyTuple_FromPair[Steal]. */
static JSValue qp_capi_tic_tuple_from_pair(JSContext *ctx, JSValueConst a, JSValueConst b,
                                           BOOL steal)
{
    PyObject *pa, *pb, *res;
    JSValue r;

    if (qp_capi_init(ctx) < 0)
        return JS_EXCEPTION;
    pa = qp_capi_new_ref(ctx, a);
    if (!pa)
        return JS_EXCEPTION;
    pb = qp_capi_new_ref(ctx, b);
    if (!pb) {
        Py_DECREF(pa);
        return JS_EXCEPTION;
    }
    if (steal) {
        res = _PyTuple_FromPairSteal(pa, pb);
    } else {
        res = _PyTuple_FromPair(pa, pb);
        Py_DECREF(pa);
        Py_DECREF(pb);
    }
    if (!res)
        return JS_EXCEPTION;
    r = qp_capi_to_js(ctx, res);
    Py_DECREF(res);
    return r;
}

/* Resize a tuple only referenced by the caller (the mirror may move, as a
   realloc in CPython).  Items beyond the new size are released. */
int _PyTuple_Resize(PyObject **pv, Py_ssize_t newsize)
{
    PyObject *v = *pv, *np;
    Py_ssize_t i, oldsize, n;

    if (v == NULL || !Py_IS_TYPE(v, &PyTuple_Type) ||
        (Py_SIZE(v) != 0 && Py_REFCNT(v) != 1) || newsize < 0) {
        *pv = NULL;
        Py_XDECREF(v);
        PyErr_BadInternalCall();
        return -1;
    }
    oldsize = Py_SIZE(v);
    if (oldsize == newsize)
        return 0;
    if (newsize == 0) {
        Py_DECREF(v);
        *pv = qp_capi_tuple_empty();
        return 0;
    }
    np = qp_capi_tuple_alloc(newsize);
    if (!np) {
        *pv = NULL;
        Py_DECREF(v);
        return -1;
    }
    n = oldsize < newsize ? oldsize : newsize;
    for (i = 0; i < n; i++) {
        ((PyTupleObject *)np)->ob_item[i] = ((PyTupleObject *)v)->ob_item[i];
        ((PyTupleObject *)v)->ob_item[i] = NULL;
    }
    ((PyTupleObject *)np)->ob_hash = -1;
    Py_DECREF(v);   /* releases the items beyond newsize */
    *pv = np;
    return 0;
}

/* ========================================================================= */
/* LIST kind                                                                  */
/* ========================================================================= */

static int qp_capi_list_init(JSContext *ctx, PyObject *op, JSValueConst v)
{
    PyListObject *l = (PyListObject *)op;
    QP_CAPI_HEAD(op)->flags |= _Py_QP_LIVE_SIZE_FLAG;
    l->_qp_items = NULL;
    l->_qp_nitems = 0;
    return 0;
}

static void qp_capi_list_clear(JSRuntime *rt, PyObject *op)
{
    PyListObject *l = (PyListObject *)op;
    if (l->_qp_items) {
        js_free_rt(rt, l->_qp_items);
        l->_qp_items = NULL;
    }
    l->_qp_nitems = 0;
}

/* listobject.c qp_lst_set_count: count and `length` of a fast array. */
static inline void qp_capi_list_set_count(JSObject *p, uint32_t n)
{
    p->u.array.count = n;
    p->prop[0].u.value = JS_MKVAL(JS_TAG_INT, n);
}

/* JS array of a list mirror in canonical fast form (count == length). */
static JSObject *qp_capi_list_obj(JSContext *ctx, PyObject *op, BOOL mutate)
{
    JSValue v = qp_capi_e1_jv(ctx, op);
    JSObject *p;
    if (JS_IsException(v))
        return NULL;
    p = JS_VALUE_GET_OBJ(v);
    if ((mutate ? qp_lst_mfast(ctx, p) : qp_lst_fast(ctx, p)) < 0)
        return NULL;
    return p;
}

static Py_ssize_t qp_capi_list_size(PyObject *op)
{
    QPCapiHead *h;
    if (!QP_CAPI_HAS_HEAD(op))
        return 0;
    h = QP_CAPI_HEAD(op);
    if (QP_IS_NULL(h->v) || (h->flags & QP_CAPI_F_ORPHAN))
        return 0;
    return qp_list_size(h->v);
}

static void qp_capi_list_set_size(PyObject *op, Py_ssize_t n)
{
    JSContext *ctx = qp_capi.ctx;
    JSObject *p = qp_capi_list_obj(ctx, op, TRUE);
    uint32_t count, i;
    JSValue *items;

    if (!p)
        return;
    if (n < 0)
        n = 0;
    count = p->u.array.count;
    if ((size_t)n < count) {
        /* the list is consistent before any value is released */
        items = p->u.array.u.values;
        qp_capi_list_set_count(p, (uint32_t)n);
        for (i = count; i-- > (uint32_t)n; )
            JS_FreeValue(ctx, items[i]);
    } else if ((size_t)n > count) {
        if (qp_lst_reserve(ctx, p, n) < 0)
            return;
        for (i = count; i < (uint32_t)n; i++)
            p->u.array.u.values[i] = JS_UNDEFINED;
        qp_capi_list_set_count(p, (uint32_t)n);
    }
}

static PyObject *qp_capi_list_no_items[1];

static PyObject **qp_capi_list_items(PyObject *op)
{
    JSContext *ctx = qp_capi.ctx;
    PyListObject *l = (PyListObject *)op;
    JSObject *p = qp_capi_list_obj(ctx, op, FALSE);
    Py_ssize_t i, n;

    if (!p)
        return NULL;
    n = p->u.array.count;
    if (n == 0)
        return l->_qp_items ? l->_qp_items : qp_capi_list_no_items;
    if (n > l->_qp_nitems) {
        PyObject **ni = js_realloc_rt(ctx->rt, l->_qp_items, sizeof(PyObject *) * (size_t)n);
        if (!ni) {
            JS_ThrowOutOfMemory(ctx);
            return NULL;
        }
        l->_qp_items = ni;
        l->_qp_nitems = n;
    }
    for (i = 0; i < n; i++) {
        PyObject *item;
        if ((size_t)i >= p->u.array.count) {     /* cannot shrink here; stay safe */
            l->_qp_items[i] = Py_None;
            continue;
        }
        item = qp_capi_borrow(ctx, op, QP_CAPI_PIN_INDEX(i), p->u.array.u.values[i]);
        if (!item)
            return NULL;
        l->_qp_items[i] = item;
    }
    return l->_qp_items;
}

/* PyList_GET_ITEM: borrowed, pinned by the list mirror for slot i. */
PyObject *_PyQp_ListGetItem(PyObject *op, Py_ssize_t index)
{
    JSContext *ctx = qp_capi.ctx;
    JSObject *p = qp_capi_list_obj(ctx, op, FALSE);
    if (!p || index < 0 || (size_t)index >= p->u.array.count)
        return NULL;
    return qp_capi_borrow(ctx, op, QP_CAPI_PIN_INDEX(index), p->u.array.u.values[index]);
}

/* Store value (stolen; NULL = None placeholder) in slot i.  The mirror of
   the previous item pinned in that slot is released when `release_old`,
   else handed over to the caller (PyList_SET_ITEM does not release the
   old item: CPython transfers the list's reference).  -1 + exception (value
   released) when i is out of range or on failure. */
static int qp_capi_list_store(JSContext *ctx, PyObject *op, Py_ssize_t i, PyObject *value,
                              BOOL release_old)
{
    JSObject *p = qp_capi_list_obj(ctx, op, TRUE);
    JSValue nv, old;
    PyObject *oldpin;
    uint64_t key = QP_CAPI_PIN_INDEX(i);

    if (!p)
        goto fail;
    if (i < 0 || (size_t)i >= p->u.array.count) {
        PyErr_SetString(PyExc_IndexError, "list assignment index out of range");
        goto fail;
    }
    nv = value ? qp_capi_to_js(ctx, value) : JS_UNDEFINED;
    if (JS_IsException(nv))
        goto fail;
    oldpin = qp_capi_pin_get(op, key);
    if (oldpin && !release_old)
        Py_INCREF(oldpin);
    if (value)
        qp_capi_pin_set(op, key, value);
    else
        qp_capi_pin_drop(op, key);
    old = p->u.array.u.values[i];
    p->u.array.u.values[i] = nv;
    JS_FreeValue(ctx, old);
    return 0;
 fail:
    Py_XDECREF(value);
    return -1;
}

void _PyQp_ListSetItem(PyObject *op, Py_ssize_t index, PyObject *value)
{
    qp_capi_list_store(qp_capi.ctx, op, index, value, FALSE);
}

/* ---- list API ----------------------------------------------------------- */

PyObject *PyList_New(Py_ssize_t size)
{
    JSContext *ctx = qp_capi.ctx;
    if (size < 0) {
        PyErr_BadInternalCall();
        return NULL;
    }
    return qp_capi_from_js_result(ctx, qp_list_new(ctx, size));
}

Py_ssize_t PyList_Size(PyObject *op)
{
    if (!PyList_Check(op)) {
        PyErr_BadInternalCall();
        return -1;
    }
    return qp_capi_list_size(op);
}

PyObject *PyList_GetItem(PyObject *op, Py_ssize_t i)
{
    JSContext *ctx = qp_capi.ctx;
    JSObject *p;

    if (!PyList_Check(op)) {
        PyErr_BadInternalCall();
        return NULL;
    }
    p = qp_capi_list_obj(ctx, op, FALSE);
    if (!p)
        return NULL;
    if (i < 0 || (size_t)i >= p->u.array.count) {
        PyErr_SetString(PyExc_IndexError, "list index out of range");
        return NULL;
    }
    return qp_capi_borrow(ctx, op, QP_CAPI_PIN_INDEX(i), p->u.array.u.values[i]);
}

PyObject *PyList_GetItemRef(PyObject *op, Py_ssize_t i)
{
    JSContext *ctx = qp_capi.ctx;
    JSObject *p;

    if (!PyList_Check(op)) {
        PyErr_SetString(PyExc_TypeError, "expected a list");
        return NULL;
    }
    p = qp_capi_list_obj(ctx, op, FALSE);
    if (!p)
        return NULL;
    if (i < 0 || (size_t)i >= p->u.array.count) {
        PyErr_SetString(PyExc_IndexError, "list index out of range");
        return NULL;
    }
    return qp_capi_new_ref(ctx, p->u.array.u.values[i]);
}

int PyList_SetItem(PyObject *op, Py_ssize_t i, PyObject *newitem)
{
    if (!PyList_Check(op)) {
        Py_XDECREF(newitem);
        PyErr_BadInternalCall();
        return -1;
    }
    return qp_capi_list_store(qp_capi.ctx, op, i, newitem, TRUE);
}

int PyList_Insert(PyObject *op, Py_ssize_t where, PyObject *newitem)
{
    JSContext *ctx = qp_capi.ctx;
    JSValue l, v;

    if (!PyList_Check(op) || newitem == NULL) {
        PyErr_BadInternalCall();
        return -1;
    }
    l = qp_capi_e1_jv(ctx, op);
    if (JS_IsException(l))
        return -1;
    v = qp_capi_value(ctx, newitem);
    if (JS_IsException(v))
        return -1;
    return qp_list_insert(ctx, l, where, v);
}

int PyList_Append(PyObject *op, PyObject *newitem)
{
    JSContext *ctx = qp_capi.ctx;
    JSValue l, v;

    if (!PyList_Check(op) || newitem == NULL) {
        PyErr_BadInternalCall();
        return -1;
    }
    l = qp_capi_e1_jv(ctx, op);
    if (JS_IsException(l))
        return -1;
    v = qp_capi_value(ctx, newitem);
    if (JS_IsException(v))
        return -1;
    return qp_list_append(ctx, l, v);
}

/* _PyList_AppendTakeRef (pycore_list.h inline over this): steals newitem;
   it stays pinned in its slot like an item stored by PyList_SET_ITEM. */
int _PyList_AppendTakeRefListResize(PyListObject *self, PyObject *newitem)
{
    JSContext *ctx = qp_capi.ctx;
    PyObject *op = (PyObject *)self;
    JSObject *p;
    JSValue v;
    uint32_t n;

    p = qp_capi_list_obj(ctx, op, TRUE);
    if (!p)
        goto fail;
    v = qp_capi_to_js(ctx, newitem);
    if (JS_IsException(v))
        goto fail;
    n = p->u.array.count;
    if (qp_lst_push(ctx, p, v) < 0)
        goto fail;
    qp_capi_pin_set(op, QP_CAPI_PIN_INDEX(n), newitem);
    return 0;
 fail:
    Py_DECREF(newitem);
    return -1;
}

PyObject *PyList_GetSlice(PyObject *op, Py_ssize_t ilow, Py_ssize_t ihigh)
{
    JSContext *ctx = qp_capi.ctx;
    JSObject *p;
    Py_ssize_t n;

    if (!PyList_Check(op)) {
        PyErr_BadInternalCall();
        return NULL;
    }
    p = qp_capi_list_obj(ctx, op, FALSE);
    if (!p)
        return NULL;
    n = p->u.array.count;
    if (ilow < 0)
        ilow = 0;
    else if (ilow > n)
        ilow = n;
    if (ihigh < ilow)
        ihigh = ilow;
    else if (ihigh > n)
        ihigh = n;
    return qp_capi_from_js_result(ctx, qp_lst_slice(ctx, p, ilow, ihigh));
}

int PyList_SetSlice(PyObject *op, Py_ssize_t ilow, Py_ssize_t ihigh, PyObject *v)
{
    JSContext *ctx = qp_capi.ctx;
    JSValue l, jv;

    if (!PyList_Check(op)) {
        PyErr_BadInternalCall();
        return -1;
    }
    l = qp_capi_e1_jv(ctx, op);
    if (JS_IsException(l))
        return -1;
    jv = qp_capi_e1_opt(ctx, v);
    if (JS_IsException(jv))
        return -1;
    return qp_lst_ass_slice(ctx, l, ilow, ihigh, jv);
}

int PyList_Sort(PyObject *v)
{
    JSContext *ctx = qp_capi.ctx;
    JSValue l;

    if (v == NULL || !PyList_Check(v)) {
        PyErr_BadInternalCall();
        return -1;
    }
    l = qp_capi_e1_jv(ctx, v);
    if (JS_IsException(l))
        return -1;
    return qp_list_sort(ctx, l, QP_NULL, 0);
}

int PyList_Reverse(PyObject *v)
{
    JSContext *ctx = qp_capi.ctx;
    JSObject *p;

    if (v == NULL || !PyList_Check(v)) {
        PyErr_BadInternalCall();
        return -1;
    }
    p = qp_capi_list_obj(ctx, v, TRUE);
    if (!p)
        return -1;
    if (p->u.array.count > 1)
        qp_sort_reverse(p->u.array.u.values, p->u.array.count);
    return 0;
}

PyObject *PyList_AsTuple(PyObject *v)
{
    JSContext *ctx = qp_capi.ctx;
    JSValue l;

    if (v == NULL || !PyList_Check(v)) {
        PyErr_BadInternalCall();
        return NULL;
    }
    l = qp_capi_e1_jv(ctx, v);
    if (JS_IsException(l))
        return NULL;
    return qp_capi_from_js_result(ctx, qp_list_as_tuple(ctx, l));
}

int PyList_Extend(PyObject *self, PyObject *iterable)
{
    JSContext *ctx = qp_capi.ctx;
    JSValue l, it;

    if (!PyList_Check(self)) {
        PyErr_BadInternalCall();
        return -1;
    }
    if (iterable == NULL) {
        PyErr_BadInternalCall();
        return -1;
    }
    l = qp_capi_e1_jv(ctx, self);
    if (JS_IsException(l))
        return -1;
    it = qp_capi_value(ctx, iterable);
    if (JS_IsException(it))
        return -1;
    return qp_list_extend(ctx, l, it);
}

int PyList_Clear(PyObject *self)
{
    JSContext *ctx = qp_capi.ctx;
    JSValue l;

    if (!PyList_Check(self)) {
        PyErr_BadInternalCall();
        return -1;
    }
    l = qp_capi_e1_jv(ctx, self);
    if (JS_IsException(l))
        return -1;
    return qp_lst_clear(ctx, JS_VALUE_GET_OBJ(l));
}

/* ========================================================================= */
/* SLICE kind                                                                 */
/* ========================================================================= */

/* One member of a slice (start/stop/step are read-only members). */
static PyObject *qp_capi_slice_member(JSContext *ctx, JSValueConst v, const char *name)
{
    JSAtom a = JS_NewAtom(ctx, name);
    JSValue r;
    if (a == JS_ATOM_NULL)
        return NULL;
    r = qp_getattr(ctx, v, a);
    JS_FreeAtom(ctx, a);
    return qp_capi_from_js_result(ctx, r);
}

static int qp_capi_slice_init(JSContext *ctx, PyObject *op, JSValueConst v)
{
    PySliceObject *s = (PySliceObject *)op;

    if (!qp_slice_check(v))
        return 0;
    s->start = qp_capi_slice_member(ctx, v, "start");
    if (!s->start)
        return -1;
    s->stop = qp_capi_slice_member(ctx, v, "stop");
    if (!s->stop)
        return -1;
    s->step = qp_capi_slice_member(ctx, v, "step");
    if (!s->step)
        return -1;
    return 0;
}

static int qp_capi_slice_traverse(PyObject *op, visitproc visit, void *arg)
{
    PySliceObject *s = (PySliceObject *)op;
    Py_VISIT(s->start);
    Py_VISIT(s->stop);
    Py_VISIT(s->step);
    return 0;
}

static void qp_capi_slice_clear(JSRuntime *rt, PyObject *op)
{
    PySliceObject *s = (PySliceObject *)op;
    Py_CLEAR(s->start);
    Py_CLEAR(s->stop);
    Py_CLEAR(s->step);
}

PyObject *PySlice_New(PyObject *start, PyObject *stop, PyObject *step)
{
    JSContext *ctx = qp_capi.ctx;
    JSValue a = JS_UNDEFINED, b = JS_UNDEFINED, c = JS_UNDEFINED;

    if (start && JS_IsException(a = qp_capi_value(ctx, start)))
        return NULL;
    if (stop && JS_IsException(b = qp_capi_value(ctx, stop)))
        return NULL;
    if (step && JS_IsException(c = qp_capi_value(ctx, step)))
        return NULL;
    return qp_capi_from_js_result(ctx, qp_slice_new(ctx, a, b, c));
}

PyObject *_PySlice_FromIndices(Py_ssize_t istart, Py_ssize_t istop)
{
    JSContext *ctx = qp_capi.ctx;
    JSValue a, b, s;

    a = qp_long_from_int64(ctx, istart);
    if (JS_IsException(a))
        return NULL;
    b = qp_long_from_int64(ctx, istop);
    if (JS_IsException(b)) {
        JS_FreeValue(ctx, a);
        return NULL;
    }
    s = qp_slice_new(ctx, a, b, JS_UNDEFINED);
    JS_FreeValue(ctx, a);
    JS_FreeValue(ctx, b);
    return qp_capi_from_js_result(ctx, s);
}

int PySlice_Unpack(PyObject *slice, Py_ssize_t *start, Py_ssize_t *stop, Py_ssize_t *step)
{
    JSContext *ctx = qp_capi.ctx;
    JSValue s;

    if (!PySlice_Check(slice)) {
        PyErr_BadInternalCall();
        return -1;
    }
    s = qp_capi_value(ctx, slice);
    if (JS_IsException(s))
        return -1;
    return qp_slice_unpack(ctx, s, start, stop, step);
}

Py_ssize_t PySlice_AdjustIndices(Py_ssize_t length, Py_ssize_t *start, Py_ssize_t *stop,
                                 Py_ssize_t step)
{
    return qp_slice_adjust_indices(length, start, stop, step);
}

int PySlice_GetIndices(PyObject *_r, Py_ssize_t length, Py_ssize_t *start, Py_ssize_t *stop,
                       Py_ssize_t *step)
{
    PySliceObject *r = (PySliceObject *)_r;

    /* XXX support long ints */
    if (r->step == Py_None) {
        *step = 1;
    } else {
        if (!PyLong_Check(r->step))
            return -1;
        *step = PyLong_AsSsize_t(r->step);
    }
    if (r->start == Py_None) {
        *start = *step < 0 ? length - 1 : 0;
    } else {
        if (!PyLong_Check(r->start))
            return -1;
        *start = PyLong_AsSsize_t(r->start);
        if (*start < 0)
            *start += length;
    }
    if (r->stop == Py_None) {
        *stop = *step < 0 ? -1 : length;
    } else {
        if (!PyLong_Check(r->stop))
            return -1;
        *stop = PyLong_AsSsize_t(r->stop);
        if (*stop < 0)
            *stop += length;
    }
    if (*stop > length)
        return -1;
    if (*start >= length)
        return -1;
    if (*step == 0)
        return -1;
    return 0;
}

int (PySlice_GetIndicesEx)(PyObject *slice, Py_ssize_t length, Py_ssize_t *start,
                           Py_ssize_t *stop, Py_ssize_t *step, Py_ssize_t *slicelength)
{
    if (PySlice_Unpack(slice, start, stop, step) < 0)
        return -1;
    *slicelength = PySlice_AdjustIndices(length, start, stop, *step);
    return 0;
}

int _PySlice_GetLongIndices(PySliceObject *self, PyObject *length, PyObject **start_ptr,
                            PyObject **stop_ptr, PyObject **step_ptr)
{
    JSContext *ctx = qp_capi.ctx;
    JSValue s, len, a, b, c;

    s = qp_capi_value(ctx, (PyObject *)self);
    if (JS_IsException(s))
        return -1;
    len = qp_capi_value(ctx, length);
    if (JS_IsException(len))
        return -1;
    if (qp_slice_get_long_indices(ctx, s, len, &a, &b, &c) < 0)
        return -1;
    *start_ptr = qp_capi_steal(ctx, a);
    *stop_ptr = qp_capi_steal(ctx, b);
    *step_ptr = qp_capi_steal(ctx, c);
    if (!*start_ptr || !*stop_ptr || !*step_ptr) {
        Py_CLEAR(*start_ptr);
        Py_CLEAR(*stop_ptr);
        Py_CLEAR(*step_ptr);
        return -1;
    }
    return 0;
}

/* ========================================================================= */
/* CELL kind                                                                  */
/* ========================================================================= */

PyObject *PyCell_New(PyObject *obj)
{
    JSContext *ctx = qp_capi.ctx;
    JSValue cell, v;

    cell = qp_cell_new(ctx, NULL);
    if (JS_IsException(cell))
        return NULL;
    if (obj) {
        v = qp_capi_value(ctx, obj);
        if (JS_IsException(v) || qp_cell_set(ctx, cell, v) < 0) {
            JS_FreeValue(ctx, cell);
            return NULL;
        }
    }
    return qp_capi_steal(ctx, cell);
}

PyObject *PyCell_Get(PyObject *op)
{
    JSContext *ctx = qp_capi.ctx;
    JSValue c, v;

    if (!PyCell_Check(op)) {
        PyErr_BadInternalCall();
        return NULL;
    }
    c = qp_capi_e1_jv(ctx, op);
    if (JS_IsException(c))
        return NULL;
    v = qp_cell_get(ctx, c);
    if (QP_IS_NULL(v))
        return NULL;
    return qp_capi_steal(ctx, v);
}

int PyCell_Set(PyObject *op, PyObject *value)
{
    JSContext *ctx = qp_capi.ctx;
    JSValue c, v;

    if (!PyCell_Check(op)) {
        PyErr_BadInternalCall();
        return -1;
    }
    c = qp_capi_e1_jv(ctx, op);
    if (JS_IsException(c))
        return -1;
    v = qp_capi_e1_opt(ctx, value);
    if (JS_IsException(v))
        return -1;
    return qp_cell_set(ctx, c, v);
}

/* PyCell_GET: borrowed content (NULL if empty), pinned by the cell. */
PyObject *_PyQp_CellGetBorrowed(PyObject *op)
{
    JSContext *ctx = qp_capi.ctx;
    JSValue c, v;
    PyObject *res;

    c = qp_capi_e1_jv(ctx, op);
    if (JS_IsException(c))
        return NULL;
    v = qp_cell_get(ctx, c);
    if (QP_IS_NULL(v)) {
        qp_capi_pin_drop(op, QP_CAPI_PIN_NAMED(QP_CAPI_ROLE_CELL));
        return NULL;
    }
    res = qp_capi_borrow(ctx, op, QP_CAPI_PIN_NAMED(QP_CAPI_ROLE_CELL), v);
    JS_FreeValue(ctx, v);
    return res;
}

/* PyCell_SET: steals value; the previous content is not released (the
   reference the cell held passes to the caller, as in CPython). */
void _PyQp_CellSetSteal(PyObject *op, PyObject *value)
{
    JSContext *ctx = qp_capi.ctx;
    uint64_t key = QP_CAPI_PIN_NAMED(QP_CAPI_ROLE_CELL);
    PyObject *oldpin;
    JSValue c, v;

    c = qp_capi_e1_jv(ctx, op);
    if (JS_IsException(c))
        goto fail;
    v = qp_capi_e1_opt(ctx, value);
    if (JS_IsException(v) || qp_cell_set(ctx, c, v) < 0)
        goto fail;
    oldpin = qp_capi_pin_get(op, key);
    if (oldpin)
        Py_INCREF(oldpin);
    if (value)
        qp_capi_pin_set(op, key, value);
    else
        qp_capi_pin_drop(op, key);
    return;
 fail:
    Py_XDECREF(value);
}

/* ========================================================================= */
/* Kind registration (called by qp_capi_init_containers, capi_dict.c)         */
/* ========================================================================= */

static void qp_capi_init_sequence_kinds(void)
{
    qp_capi.kind_ops[QP_CAPI_K_TUPLE] = (QPCapiKindOps){
        .name = "tuple",
        .size = qp_capi_tuple_size,
        .init = qp_capi_tuple_init,
        .to_js = qp_capi_tuple_to_js,
        .refresh = qp_capi_tuple_refresh,
        .traverse = qp_capi_tuple_traverse,
        .clear = qp_capi_tuple_clear,
    };
    qp_capi.kind_ops[QP_CAPI_K_LIST] = (QPCapiKindOps){
        .name = "list",
        .init = qp_capi_list_init,
        .clear = qp_capi_list_clear,
    };
    qp_capi.kind_ops[QP_CAPI_K_SLICE] = (QPCapiKindOps){
        .name = "slice",
        .init = qp_capi_slice_init,
        .traverse = qp_capi_slice_traverse,
        .clear = qp_capi_slice_clear,
    };
    qp_capi.kind_ops[QP_CAPI_K_CELL] = (QPCapiKindOps){
        .name = "cell",
    };
}
