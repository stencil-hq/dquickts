/*
 * QuickJS heap snapshots: control flow classes and modules
 *
 * Generators, async generators, promises and their resolving functions,
 * async function resolving functions, async-from-sync iterators, iterator
 * helpers, array/string/regexp string/for-in iterators, Proxy,
 * DisposableStack and AsyncDisposableStack (with the state of a running
 * disposeAsync()), and module records. Included by quickjs-snapshot.c.
 *
 * Every object payload of this family is a pointer to a C structure that
 * is only NULL if the object construction failed half-way, so it starts
 * with a presence byte (except for-in iterators, whose payload is never
 * NULL). Heap frames (JSAsyncFunctionState) are cells of their own and are
 * referenced with snap_put_ref(SNAP_CELL_FRAME). Nothing here needs the
 * content of another cell, so no fixup is used: the reader attaches the
 * structure to its object as soon as it is allocated, with every field
 * set to an empty value, and calls snap_payload_ready() so that a failed
 * read releases it with the class finalizer.
 */

/* All the code pointers that the classes of this family store in the heap
   (resolving functions of async generators, Promise combinators,
   async-from-sync iterators, Array.fromAsync, modules, and the promise and
   dynamic import jobs) are already in snap_builtin_natives[]. C module
   init functions come from the host natives. */
static int snap_add_control_natives(JSRuntime *rt, SnapNatives *n)
{
    return 0;
}

/* number of entries of an array that is about to be read: each entry
   takes at least one byte, which bounds the allocation */
static int snap_get_count(SnapReader *r, uint32_t *pn)
{
    if (snap_get_leb(r, pn))
        return -1;
    if (*pn > r->bc.buf_end - r->bc.ptr)
        return snap_error(r, "truncated snapshot");
    return 0;
}

static BOOL snap_put_present(SnapWriter *w, const void *ptr)
{
    bc_put_u8(&w->bc, ptr != NULL);
    return ptr != NULL;
}

static int snap_get_present(SnapReader *r, BOOL *ppresent)
{
    uint8_t v;
    if (bc_get_u8(&r->bc, &v))
        return -1;
    *ppresent = v;
    if (v > 1)
        return snap_error(r, "invalid object payload");
    return 0;
}

static int snap_put_values(SnapWriter *w, const JSValue *tab, int n)
{
    int i;
    for(i = 0; i < n; i++) {
        if (snap_put_value(w, tab[i]))
            return -1;
    }
    return 0;
}

/* the slots must be initialized: each one stays valid on error */
static int snap_get_values(SnapReader *r, JSValue *tab, int n)
{
    int i;
    for(i = 0; i < n; i++) {
        if (snap_get_value(r, &tab[i]))
            return -1;
    }
    return 0;
}

static void snap_init_values(JSValue *tab, int n)
{
    int i;
    for(i = 0; i < n; i++)
        tab[i] = JS_UNDEFINED;
}

/* take a reference to a frame cell (may be NULL) */
static JSAsyncFunctionState *snap_dup_frame(JSAsyncFunctionState *s)
{
    if (s)
        js_rc(s)->ref_count++;
    return s;
}

/*******************************************************************/
/* writer */

static int snap_write_generator(SnapWriter *w, JSGeneratorData *s)
{
    if (!snap_put_present(w, s))
        return 0;
    if (s->state == JS_GENERATOR_STATE_EXECUTING) {
        JS_ThrowTypeError(w->ctx, "snapshot: running generator");
        return -1;
    }
    bc_put_u8(&w->bc, s->state);
    return snap_put_ref(w, SNAP_CELL_FRAME, s->func_state);
}

static int snap_write_async_generator(SnapWriter *w, JSAsyncGeneratorData *s)
{
    struct list_head *el;
    uint32_t n;

    if (!snap_put_present(w, s))
        return 0;
    /* EXECUTING is also the state of an async generator waiting for an
       await: a frame that is really running is rejected by the frame
       writer */
    bc_put_u8(&w->bc, s->state);
    if (snap_put_ref(w, SNAP_CELL_FRAME, s->func_state))
        return -1;
    n = 0;
    list_for_each(el, &s->queue)
        n++;
    bc_put_leb128(&w->bc, n);
    list_for_each(el, &s->queue) {
        JSAsyncGeneratorRequest *req = list_entry(el, JSAsyncGeneratorRequest, link);
        bc_put_u8(&w->bc, req->completion_type);
        if (snap_put_value(w, req->result) ||
            snap_put_value(w, req->promise) ||
            snap_put_values(w, req->resolving_funcs, 2))
            return -1;
    }
    return 0;
}

static int snap_write_promise(SnapWriter *w, JSPromiseData *s)
{
    struct list_head *el;
    uint32_t n;
    int i;

    if (!snap_put_present(w, s))
        return 0;
    bc_put_u8(&w->bc, s->promise_state);
    bc_put_u8(&w->bc, s->is_handled);
    if (snap_put_value(w, s->promise_result))
        return -1;
    for(i = 0; i < 2; i++) {
        n = 0;
        list_for_each(el, &s->promise_reactions[i])
            n++;
        bc_put_leb128(&w->bc, n);
        list_for_each(el, &s->promise_reactions[i]) {
            JSPromiseReactionData *rd = list_entry(el, JSPromiseReactionData, link);
            if (snap_put_values(w, rd->resolving_funcs, 2) ||
                snap_put_value(w, rd->handler) ||
                snap_put_value(w, rd->async_context))
                return -1;
        }
    }
    return 0;
}

/* the 'already resolved' record shared by a resolve/reject pair is a
   cell (SNAP_CELL_RESOLVED) */
static int snap_write_promise_function(SnapWriter *w, JSPromiseFunctionData *s)
{
    if (!snap_put_present(w, s))
        return 0;
    if (snap_put_value(w, s->promise) ||
        snap_put_ref(w, SNAP_CELL_RESOLVED, s->presolved))
        return -1;
    return 0;
}

static int snap_write_resolved(SnapWriter *w, JSPromiseFunctionDataResolved *sr)
{
    bc_put_u8(&w->bc, sr->already_resolved);
    return 0;
}

static JSPromiseFunctionDataResolved *snap_new_resolved(SnapReader *r)
{
    JSPromiseFunctionDataResolved *sr = js_malloc(r->ctx, sizeof(*sr));
    if (!sr)
        return NULL;
    sr->ref_count = 1;
    sr->already_resolved = FALSE;
    return sr;
}

static int snap_read_resolved(SnapReader *r, JSPromiseFunctionDataResolved *sr)
{
    uint8_t already_resolved;
    if (bc_get_u8(&r->bc, &already_resolved))
        return -1;
    sr->already_resolved = already_resolved != 0;
    return 0;
}

static int snap_write_iterator_concat(SnapWriter *w, JSIteratorConcatData *it)
{
    if (!snap_put_present(w, it))
        return 0;
    /* values[] before 'index' were released */
    bc_put_leb128(&w->bc, it->index);
    bc_put_leb128(&w->bc, it->count);
    bc_put_u8(&w->bc, it->running);
    if (snap_put_value(w, it->iter) ||
        snap_put_value(w, it->next))
        return -1;
    return snap_put_values(w, it->values + it->index, it->count - it->index);
}

static int snap_write_iterator_helper(SnapWriter *w, JSIteratorHelperData *it)
{
    if (!snap_put_present(w, it))
        return 0;
    bc_put_u8(&w->bc, it->kind);
    bc_put_u8(&w->bc, it->executing);
    bc_put_u8(&w->bc, it->done);
    bc_put_u8(&w->bc, it->allow_partial);
    bc_put_u64(&w->bc, it->count);
    if (snap_put_value(w, it->obj) ||
        snap_put_value(w, it->next) ||
        snap_put_value(w, it->func) ||
        snap_put_value(w, it->inner) ||
        snap_put_value(w, it->inner_next))
        return -1;
    bc_put_leb128(&w->bc, it->buf_count);
    return snap_put_values(w, it->buf, it->buf_count);
}

static int snap_write_iterator_zip(SnapWriter *w, JSIteratorZipData *it)
{
    int i;

    if (!snap_put_present(w, it))
        return 0;
    bc_put_leb128(&w->bc, it->count);
    bc_put_u8(&w->bc, it->mode);
    bc_put_u8(&w->bc, it->keyed);
    bc_put_u8(&w->bc, it->running);
    bc_put_u8(&w->bc, it->done);
    bc_put_u8(&w->bc, it->started);
    for(i = 0; i < it->count; i++) {
        bc_put_u8(&w->bc, it->state[i]);
        if (snap_put_value(w, it->iters[i]) ||
            snap_put_value(w, it->nexts[i]) ||
            snap_put_value(w, it->padding[i]))
            return -1;
        if (it->keyed && bc_put_atom(&w->bc, it->keys[i]))
            return -1;
    }
    return 0;
}

static int snap_write_array_from_async(SnapWriter *w, JSArrayFromAsyncData *s)
{
    if (!snap_put_present(w, s))
        return 0;
    bc_put_u64(&w->bc, s->k);
    bc_put_u64(&w->bc, s->length);
    bc_put_leb128(&w->bc, s->state);
    bc_put_u8(&w->bc, s->mapping);
    bc_put_u8(&w->bc, s->has_iterator);
    if (snap_put_value(w, s->promise) ||
        snap_put_values(w, s->resolving_funcs, 2) ||
        snap_put_value(w, s->items) ||
        snap_put_value(w, s->ctor) ||
        snap_put_value(w, s->mapper) ||
        snap_put_value(w, s->this_arg) ||
        snap_put_value(w, s->iterator) ||
        snap_put_value(w, s->next_method) ||
        snap_put_value(w, s->array) ||
        snap_put_value(w, s->array_like) ||
        snap_put_value(w, s->value) ||
        snap_put_value(w, s->error))
        return -1;
    return 0;
}

static int snap_write_disposable_stack(SnapWriter *w, JSDisposableStack *s)
{
    int i;

    if (!snap_put_present(w, s))
        return 0;
    bc_put_u8(&w->bc, s->disposed | (s->needs_await << 1) |
              (s->has_awaited << 2));
    bc_put_leb128(&w->bc, s->count);
    for(i = 0; i < s->count; i++) {
        bc_put_u8(&w->bc, s->resources[i].call_kind);
        if (snap_put_value(w, s->resources[i].value) ||
            snap_put_value(w, s->resources[i].method))
            return -1;
    }
    if (snap_put_value(w, s->error) ||
        snap_put_values(w, s->resolving_funcs, 2))
        return -1;
    return 0;
}

static int snap_write_for_in_iterator(SnapWriter *w, JSForInIterator *it)
{
    uint32_t i;

    bc_put_leb128(&w->bc, it->idx);
    bc_put_leb128(&w->bc, it->atom_count);
    bc_put_u8(&w->bc, it->in_prototype_chain);
    bc_put_u8(&w->bc, it->is_array);
    if (snap_put_value(w, it->obj))
        return -1;
    if (!it->is_array) {
        for(i = 0; i < it->atom_count; i++) {
            if (bc_put_atom(&w->bc, it->tab_atom[i].atom))
                return -1;
            bc_put_u8(&w->bc, it->tab_atom[i].is_enumerable);
        }
    }
    return 0;
}

static int snap_write_control_payload(SnapWriter *w, JSObject *p)
{
    switch(p->class_id) {
    case JS_CLASS_GENERATOR:
        return snap_write_generator(w, p->u.generator_data);
    case JS_CLASS_ASYNC_GENERATOR:
        return snap_write_async_generator(w, p->u.async_generator_data);
    case JS_CLASS_PROMISE:
        return snap_write_promise(w, p->u.promise_data);
    case JS_CLASS_PROMISE_RESOLVE_FUNCTION:
    case JS_CLASS_PROMISE_REJECT_FUNCTION:
        return snap_write_promise_function(w, p->u.promise_function_data);
    case JS_CLASS_ASYNC_FUNCTION_RESOLVE:
    case JS_CLASS_ASYNC_FUNCTION_REJECT:
        return snap_put_ref(w, SNAP_CELL_FRAME, p->u.async_function_data);
    case JS_CLASS_ASYNC_FROM_SYNC_ITERATOR:
        {
            JSAsyncFromSyncIteratorData *s = p->u.async_from_sync_iterator_data;
            if (!snap_put_present(w, s))
                return 0;
            if (snap_put_value(w, s->sync_iter) ||
                snap_put_value(w, s->next_method))
                return -1;
        }
        break;
    case JS_CLASS_ITERATOR_CONCAT:
        return snap_write_iterator_concat(w, p->u.iterator_concat_data);
    case JS_CLASS_ITERATOR_HELPER:
        return snap_write_iterator_helper(w, p->u.iterator_helper_data);
    case JS_CLASS_ITERATOR_WRAP:
        {
            JSIteratorWrapData *it = p->u.iterator_wrap_data;
            if (!snap_put_present(w, it))
                return 0;
            if (snap_put_value(w, it->wrapped_iter) ||
                snap_put_value(w, it->wrapped_next))
                return -1;
        }
        break;
    case JS_CLASS_ITERATOR_ZIP:
        return snap_write_iterator_zip(w, p->u.iterator_zip_data);
    case JS_CLASS_ARRAY_FROM_ASYNC:
        return snap_write_array_from_async(w, p->u.array_from_async_data);
    case JS_CLASS_DISPOSABLE_STACK:
    case JS_CLASS_ASYNC_DISPOSABLE_STACK:
        return snap_write_disposable_stack(w, p->u.disposable_stack);
    case JS_CLASS_ARRAY_ITERATOR:
    case JS_CLASS_STRING_ITERATOR:
        {
            JSArrayIteratorData *it = p->u.array_iterator_data;
            if (!snap_put_present(w, it))
                return 0;
            bc_put_u8(&w->bc, it->kind);
            bc_put_leb128(&w->bc, it->idx);
            return snap_put_value(w, it->obj);
        }
    case JS_CLASS_REGEXP_STRING_ITERATOR:
        {
            JSRegExpStringIteratorData *it = p->u.regexp_string_iterator_data;
            if (!snap_put_present(w, it))
                return 0;
            bc_put_u8(&w->bc, it->global);
            bc_put_u8(&w->bc, it->unicode);
            bc_put_u8(&w->bc, it->done);
            if (snap_put_value(w, it->iterating_regexp) ||
                snap_put_value(w, it->iterated_string))
                return -1;
        }
        break;
    case JS_CLASS_FOR_IN_ITERATOR:
        return snap_write_for_in_iterator(w, p->u.for_in_iterator);
    case JS_CLASS_PROXY:
        {
            JSProxyData *s = p->u.proxy_data;
            if (!snap_put_present(w, s))
                return 0;
            bc_put_u8(&w->bc, s->is_func);
            bc_put_u8(&w->bc, s->is_revoked);
            if (snap_put_value(w, s->target) ||
                snap_put_value(w, s->handler))
                return -1;
        }
        break;
    default:
        return 1;
    }
    return 0;
}

/*******************************************************************/
/* reader */

static int snap_read_generator(SnapReader *r, JSObject *p)
{
    JSGeneratorData *s;
    JSAsyncFunctionState *fs;
    uint8_t state;
    BOOL present;

    if (snap_get_present(r, &present))
        return -1;
    if (!present)
        return 0;
    if (bc_get_u8(&r->bc, &state) ||
        snap_get_ref(r, SNAP_CELL_FRAME, (void **)&fs))
        return -1;
    if (state > JS_GENERATOR_STATE_COMPLETED ||
        state == JS_GENERATOR_STATE_EXECUTING ||
        (state == JS_GENERATOR_STATE_COMPLETED) != (fs == NULL))
        return snap_error(r, "invalid generator");
    s = js_malloc(r->ctx, sizeof(*s));
    if (!s)
        return -1;
    s->state = state;
    s->func_state = snap_dup_frame(fs);
    p->u.generator_data = s;
    return 0;
}

static int snap_read_async_generator(SnapReader *r, JSObject *p)
{
    JSAsyncGeneratorData *s;
    JSAsyncFunctionState *fs;
    uint8_t state, type;
    uint32_t i, n;
    BOOL present;

    if (snap_get_present(r, &present))
        return -1;
    if (!present)
        return 0;
    if (bc_get_u8(&r->bc, &state) ||
        snap_get_ref(r, SNAP_CELL_FRAME, (void **)&fs))
        return -1;
    /* no frame once the generator is completed */
    if (state > JS_ASYNC_GENERATOR_STATE_COMPLETED ||
        (state >= JS_ASYNC_GENERATOR_STATE_AWAITING_RETURN) != (fs == NULL))
        return snap_error(r, "invalid async generator");
    s = js_mallocz(r->ctx, sizeof(*s));
    if (!s)
        return -1;
    s->generator = p;
    s->state = state;
    s->func_state = snap_dup_frame(fs);
    init_list_head(&s->queue);
    p->u.async_generator_data = s;
    snap_payload_ready(r);

    if (snap_get_count(r, &n))
        return -1;
    for(i = 0; i < n; i++) {
        JSAsyncGeneratorRequest *req;
        if (bc_get_u8(&r->bc, &type))
            return -1;
        if (type > GEN_MAGIC_THROW)
            return snap_error(r, "invalid async generator request");
        req = js_mallocz(r->ctx, sizeof(*req));
        if (!req)
            return -1;
        req->completion_type = type;
        req->result = JS_UNDEFINED;
        req->promise = JS_UNDEFINED;
        snap_init_values(req->resolving_funcs, 2);
        list_add_tail(&req->link, &s->queue);
        if (snap_get_value(r, &req->result) ||
            snap_get_value(r, &req->promise) ||
            snap_get_values(r, req->resolving_funcs, 2))
            return -1;
    }
    return 0;
}

static int snap_read_promise(SnapReader *r, JSObject *p)
{
    JSPromiseData *s;
    uint8_t state, is_handled;
    uint32_t n, j;
    BOOL present;
    int i;

    if (snap_get_present(r, &present))
        return -1;
    if (!present)
        return 0;
    if (bc_get_u8(&r->bc, &state) || bc_get_u8(&r->bc, &is_handled))
        return -1;
    if (state > JS_PROMISE_REJECTED || is_handled > 1)
        return snap_error(r, "invalid promise");
    s = js_mallocz(r->ctx, sizeof(*s));
    if (!s)
        return -1;
    s->promise_state = state;
    s->is_handled = is_handled;
    s->promise_result = JS_UNDEFINED;
    for(i = 0; i < 2; i++)
        init_list_head(&s->promise_reactions[i]);
    p->u.promise_data = s;
    snap_payload_ready(r);

    if (snap_get_value(r, &s->promise_result))
        return -1;
    for(i = 0; i < 2; i++) {
        if (snap_get_count(r, &n))
            return -1;
        if (n != 0 && state != JS_PROMISE_PENDING)
            return snap_error(r, "invalid promise reactions");
        for(j = 0; j < n; j++) {
            JSPromiseReactionData *rd = js_mallocz(r->ctx, sizeof(*rd));
            if (!rd)
                return -1;
            snap_init_values(rd->resolving_funcs, 2);
            rd->handler = JS_UNDEFINED;
            rd->async_context = JS_UNDEFINED;
            list_add_tail(&rd->link, &s->promise_reactions[i]);
            if (snap_get_values(r, rd->resolving_funcs, 2) ||
                snap_get_value(r, &rd->handler) ||
                snap_get_value(r, &rd->async_context))
                return -1;
        }
    }
    return 0;
}

static int snap_read_promise_function(SnapReader *r, JSObject *p)
{
    JSPromiseFunctionData *s;
    JSPromiseFunctionDataResolved *sr;
    JSValue promise;
    BOOL present;

    if (snap_get_present(r, &present))
        return -1;
    if (!present)
        return 0;
    if (snap_get_value(r, &promise))
        return -1;
    if (snap_get_ref(r, SNAP_CELL_RESOLVED, (void **)&sr))
        goto fail;
    if (!sr) {
        snap_error(r, "invalid promise function");
        goto fail;
    }
    s = js_malloc(r->ctx, sizeof(*s));
    if (!s)
        goto fail;
    sr->ref_count++;
    s->promise = promise;
    s->presolved = sr;
    p->u.promise_function_data = s;
    return 0;
 fail:
    JS_FreeValue(r->ctx, promise);
    return -1;
}

static int snap_read_iterator_concat(SnapReader *r, JSObject *p)
{
    JSIteratorConcatData *it;
    uint32_t index, count;
    uint8_t running;
    BOOL present;

    if (snap_get_present(r, &present))
        return -1;
    if (!present)
        return 0;
    if (snap_get_leb(r, &index) || snap_get_leb(r, &count) ||
        bc_get_u8(&r->bc, &running))
        return -1;
    /* only the values from 'index' are written: each takes at least one
       byte, which bounds the allocation */
    if (index > count || (count & 1) || count > INT32_MAX / sizeof(JSValue) ||
        count - index > r->bc.buf_end - r->bc.ptr)
        return snap_error(r, "invalid iterator");
    it = js_malloc(r->ctx, sizeof(*it) + sizeof(it->values[0]) * count);
    if (!it)
        return -1;
    it->index = index;
    it->count = count;
    it->running = running != 0;
    it->iter = JS_UNDEFINED;
    it->next = JS_UNDEFINED;
    snap_init_values(it->values, count);
    p->u.iterator_concat_data = it;
    snap_payload_ready(r);
    if (snap_get_value(r, &it->iter) ||
        snap_get_value(r, &it->next) ||
        snap_get_values(r, it->values + index, count - index))
        return -1;
    return 0;
}

static int snap_read_iterator_helper(SnapReader *r, JSObject *p)
{
    JSIteratorHelperData *it;
    uint8_t kind, executing, done, allow_partial;
    uint64_t count;
    uint32_t n;
    BOOL present;

    if (snap_get_present(r, &present))
        return -1;
    if (!present)
        return 0;
    if (bc_get_u8(&r->bc, &kind) ||
        bc_get_u8(&r->bc, &executing) ||
        bc_get_u8(&r->bc, &done) ||
        bc_get_u8(&r->bc, &allow_partial) ||
        bc_get_u64(&r->bc, &count))
        return -1;
    if (kind > JS_ITERATOR_HELPER_KIND_WINDOWS)
        return snap_error(r, "invalid iterator helper");
    it = js_malloc(r->ctx, sizeof(*it));
    if (!it)
        return -1;
    it->kind = kind;
    it->executing = executing != 0;
    it->done = done != 0;
    it->allow_partial = allow_partial != 0;
    it->count = count;
    it->obj = JS_UNDEFINED;
    it->next = JS_UNDEFINED;
    it->func = JS_UNDEFINED;
    it->inner = JS_UNDEFINED;
    it->inner_next = JS_UNDEFINED;
    it->buf = NULL;
    it->buf_count = 0;
    it->buf_size = 0;
    p->u.iterator_helper_data = it;
    snap_payload_ready(r);
    if (snap_get_value(r, &it->obj) ||
        snap_get_value(r, &it->next) ||
        snap_get_value(r, &it->func) ||
        snap_get_value(r, &it->inner) ||
        snap_get_value(r, &it->inner_next) ||
        snap_get_count(r, &n))
        return -1;
    if (n > 0) {
        if (n > INT32_MAX / sizeof(JSValue))
            return snap_error(r, "invalid iterator helper");
        it->buf = js_malloc(r->ctx, sizeof(it->buf[0]) * n);
        if (!it->buf)
            return -1;
        snap_init_values(it->buf, n);
        it->buf_size = n;
        it->buf_count = n;
        if (snap_get_values(r, it->buf, n))
            return -1;
    }
    return 0;
}

static int snap_read_iterator_zip(SnapReader *r, JSObject *p)
{
    JSContext *ctx = r->ctx;
    JSIteratorZipData *it;
    uint8_t mode, keyed, running, done, started, state;
    uint32_t i, count;
    BOOL present;

    if (snap_get_present(r, &present))
        return -1;
    if (!present)
        return 0;
    if (snap_get_count(r, &count) ||
        bc_get_u8(&r->bc, &mode) ||
        bc_get_u8(&r->bc, &keyed) ||
        bc_get_u8(&r->bc, &running) ||
        bc_get_u8(&r->bc, &done) ||
        bc_get_u8(&r->bc, &started))
        return -1;
    if (mode > ZIP_MODE_STRICT || count > INT32_MAX / sizeof(JSValue) - 1)
        return snap_error(r, "invalid zip iterator");
    it = js_mallocz(ctx, sizeof(*it));
    if (!it)
        return -1;
    it->mode = mode;
    it->keyed = keyed != 0;
    it->running = running != 0;
    it->done = done != 0;
    it->started = started != 0;
    p->u.iterator_zip_data = it;
    snap_payload_ready(r);
    /* same allocations as js_iterator_zip() */
    it->padding = js_malloc(ctx, sizeof(it->padding[0]) * (count + 1));
    if (!it->padding)
        return -1;
    snap_init_values(it->padding, count + 1);
    if (count > 0) {
        it->iters = js_malloc(ctx, sizeof(it->iters[0]) * count);
        it->nexts = js_malloc(ctx, sizeof(it->nexts[0]) * count);
        it->state = js_mallocz(ctx, count);
        if (keyed)
            it->keys = js_mallocz(ctx, sizeof(it->keys[0]) * count);
        if (!it->iters || !it->nexts || !it->state || (keyed && !it->keys))
            return -1;
        snap_init_values(it->iters, count);
        snap_init_values(it->nexts, count);
        it->count = count;
    }
    for(i = 0; i < count; i++) {
        if (bc_get_u8(&r->bc, &state))
            return -1;
        if (state > ZIP_EXHAUSTED)
            return snap_error(r, "invalid zip iterator");
        it->state[i] = state;
        if (snap_get_value(r, &it->iters[i]) ||
            snap_get_value(r, &it->nexts[i]) ||
            snap_get_value(r, &it->padding[i]))
            return -1;
        if (keyed && bc_get_atom(&r->bc, &it->keys[i]))
            return -1;
    }
    return 0;
}

static int snap_read_array_from_async(SnapReader *r, JSObject *p)
{
    JSArrayFromAsyncData *s;
    uint64_t k, length;
    uint32_t state;
    uint8_t mapping, has_iterator;
    BOOL present;

    if (snap_get_present(r, &present))
        return -1;
    if (!present)
        return 0;
    if (bc_get_u64(&r->bc, &k) ||
        bc_get_u64(&r->bc, &length) ||
        snap_get_leb(r, &state) ||
        bc_get_u8(&r->bc, &mapping) ||
        bc_get_u8(&r->bc, &has_iterator))
        return -1;
    if (state > AFS_STATE_CLOSE)
        return snap_error(r, "invalid Array.fromAsync state");
    s = js_mallocz(r->ctx, sizeof(*s));
    if (!s)
        return -1;
    s->k = k;
    s->length = length;
    s->state = state;
    s->mapping = mapping != 0;
    s->has_iterator = has_iterator != 0;
    s->promise = JS_UNDEFINED;
    snap_init_values(s->resolving_funcs, 2);
    s->items = JS_UNDEFINED;
    s->ctor = JS_UNDEFINED;
    s->mapper = JS_UNDEFINED;
    s->this_arg = JS_UNDEFINED;
    s->iterator = JS_UNDEFINED;
    s->next_method = JS_UNDEFINED;
    s->array = JS_UNDEFINED;
    s->array_like = JS_UNDEFINED;
    s->value = JS_UNDEFINED;
    s->error = JS_UNDEFINED;
    p->u.array_from_async_data = s;
    snap_payload_ready(r);
    if (snap_get_value(r, &s->promise) ||
        snap_get_values(r, s->resolving_funcs, 2) ||
        snap_get_value(r, &s->items) ||
        snap_get_value(r, &s->ctor) ||
        snap_get_value(r, &s->mapper) ||
        snap_get_value(r, &s->this_arg) ||
        snap_get_value(r, &s->iterator) ||
        snap_get_value(r, &s->next_method) ||
        snap_get_value(r, &s->array) ||
        snap_get_value(r, &s->array_like) ||
        snap_get_value(r, &s->value) ||
        snap_get_value(r, &s->error))
        return -1;
    return 0;
}

static int snap_read_disposable_stack(SnapReader *r, JSObject *p)
{
    JSDisposableStack *s;
    uint32_t count, i;
    uint8_t flags, call_kind;
    BOOL present;

    if (snap_get_present(r, &present))
        return -1;
    if (!present)
        return 0;
    if (bc_get_u8(&r->bc, &flags) || snap_get_count(r, &count))
        return -1;
    if (flags > 7 || count > INT32_MAX)
        return snap_error(r, "invalid disposable stack");
    s = js_mallocz(r->ctx, sizeof(*s));
    if (!s)
        return -1;
    s->disposed = flags & 1;
    s->needs_await = (flags >> 1) & 1;
    s->has_awaited = (flags >> 2) & 1;
    s->error = JS_UNDEFINED;
    snap_init_values(s->resolving_funcs, 2);
    p->u.disposable_stack = s;
    snap_payload_ready(r);
    if (count > 0) {
        s->resources = js_malloc(r->ctx, sizeof(s->resources[0]) * count);
        if (!s->resources)
            return -1;
        s->size = count;
    }
    /* each resource is valid as soon as it is counted */
    for(i = 0; i < count; i++) {
        if (bc_get_u8(&r->bc, &call_kind))
            return -1;
        if (call_kind > JS_DISPOSE_CALL_DEFER)
            return snap_error(r, "invalid disposable resource");
        s->resources[i].call_kind = call_kind;
        s->resources[i].value = JS_UNDEFINED;
        s->resources[i].method = JS_UNDEFINED;
        s->count++;
        if (snap_get_value(r, &s->resources[i].value) ||
            snap_get_value(r, &s->resources[i].method))
            return -1;
    }
    if (snap_get_value(r, &s->error) ||
        snap_get_values(r, s->resolving_funcs, 2))
        return -1;
    return 0;
}

static int snap_read_for_in_iterator(SnapReader *r, JSObject *p)
{
    JSForInIterator *it;
    uint32_t idx, atom_count, i;
    uint8_t in_prototype_chain, is_array, is_enumerable;

    if (snap_get_leb(r, &idx) ||
        snap_get_leb(r, &atom_count) ||
        bc_get_u8(&r->bc, &in_prototype_chain) ||
        bc_get_u8(&r->bc, &is_array))
        return -1;
    if (!is_array && atom_count > r->bc.buf_end - r->bc.ptr)
        return snap_error(r, "truncated snapshot");
    it = js_malloc(r->ctx, sizeof(*it));
    if (!it)
        return -1;
    it->obj = JS_UNDEFINED;
    it->idx = idx;
    it->in_prototype_chain = in_prototype_chain != 0;
    it->is_array = is_array != 0;
    it->atom_count = is_array ? atom_count : 0;
    it->tab_atom = NULL;
    p->u.for_in_iterator = it;
    snap_payload_ready(r);
    if (snap_get_value(r, &it->obj))
        return -1;
    if (!is_array && atom_count > 0) {
        it->tab_atom = js_mallocz(r->ctx, sizeof(it->tab_atom[0]) * atom_count);
        if (!it->tab_atom)
            return -1;
        it->atom_count = atom_count; /* JS_ATOM_NULL entries can be freed */
        for(i = 0; i < atom_count; i++) {
            if (bc_get_atom(&r->bc, &it->tab_atom[i].atom) ||
                bc_get_u8(&r->bc, &is_enumerable))
                return -1;
            it->tab_atom[i].is_enumerable = is_enumerable != 0;
        }
    }
    return 0;
}

static int snap_read_control_payload(SnapReader *r, JSObject *p)
{
    BOOL present;

    switch(p->class_id) {
    case JS_CLASS_GENERATOR:
        return snap_read_generator(r, p);
    case JS_CLASS_ASYNC_GENERATOR:
        return snap_read_async_generator(r, p);
    case JS_CLASS_PROMISE:
        return snap_read_promise(r, p);
    case JS_CLASS_PROMISE_RESOLVE_FUNCTION:
    case JS_CLASS_PROMISE_REJECT_FUNCTION:
        return snap_read_promise_function(r, p);
    case JS_CLASS_ASYNC_FUNCTION_RESOLVE:
    case JS_CLASS_ASYNC_FUNCTION_REJECT:
        {
            JSAsyncFunctionState *s;
            if (snap_get_ref(r, SNAP_CELL_FRAME, (void **)&s))
                return -1;
            p->u.async_function_data = snap_dup_frame(s);
        }
        break;
    case JS_CLASS_ASYNC_FROM_SYNC_ITERATOR:
        {
            JSAsyncFromSyncIteratorData *s;
            if (snap_get_present(r, &present))
                return -1;
            if (!present)
                break;
            s = js_malloc(r->ctx, sizeof(*s));
            if (!s)
                return -1;
            s->sync_iter = JS_UNDEFINED;
            s->next_method = JS_UNDEFINED;
            p->u.async_from_sync_iterator_data = s;
            snap_payload_ready(r);
            if (snap_get_value(r, &s->sync_iter) ||
                snap_get_value(r, &s->next_method))
                return -1;
        }
        break;
    case JS_CLASS_ITERATOR_CONCAT:
        return snap_read_iterator_concat(r, p);
    case JS_CLASS_ITERATOR_HELPER:
        return snap_read_iterator_helper(r, p);
    case JS_CLASS_ITERATOR_WRAP:
        {
            JSIteratorWrapData *it;
            if (snap_get_present(r, &present))
                return -1;
            if (!present)
                break;
            it = js_malloc(r->ctx, sizeof(*it));
            if (!it)
                return -1;
            it->wrapped_iter = JS_UNDEFINED;
            it->wrapped_next = JS_UNDEFINED;
            p->u.iterator_wrap_data = it;
            snap_payload_ready(r);
            if (snap_get_value(r, &it->wrapped_iter) ||
                snap_get_value(r, &it->wrapped_next))
                return -1;
        }
        break;
    case JS_CLASS_ITERATOR_ZIP:
        return snap_read_iterator_zip(r, p);
    case JS_CLASS_ARRAY_FROM_ASYNC:
        return snap_read_array_from_async(r, p);
    case JS_CLASS_DISPOSABLE_STACK:
    case JS_CLASS_ASYNC_DISPOSABLE_STACK:
        return snap_read_disposable_stack(r, p);
    case JS_CLASS_ARRAY_ITERATOR:
    case JS_CLASS_STRING_ITERATOR:
        {
            JSArrayIteratorData *it;
            uint8_t kind;
            uint32_t idx;
            if (snap_get_present(r, &present))
                return -1;
            if (!present)
                break;
            if (bc_get_u8(&r->bc, &kind) || snap_get_leb(r, &idx))
                return -1;
            if (kind > JS_ITERATOR_KIND_KEY_AND_VALUE)
                return snap_error(r, "invalid array iterator");
            it = js_malloc(r->ctx, sizeof(*it));
            if (!it)
                return -1;
            it->obj = JS_UNDEFINED;
            it->kind = kind;
            it->idx = idx;
            p->u.array_iterator_data = it;
            snap_payload_ready(r);
            return snap_get_value(r, &it->obj);
        }
    case JS_CLASS_REGEXP_STRING_ITERATOR:
        {
            JSRegExpStringIteratorData *it;
            uint8_t global, unicode, done;
            if (snap_get_present(r, &present))
                return -1;
            if (!present)
                break;
            if (bc_get_u8(&r->bc, &global) ||
                bc_get_u8(&r->bc, &unicode) ||
                bc_get_u8(&r->bc, &done))
                return -1;
            it = js_malloc(r->ctx, sizeof(*it));
            if (!it)
                return -1;
            it->global = global != 0;
            it->unicode = unicode != 0;
            it->done = done != 0;
            it->iterating_regexp = JS_UNDEFINED;
            it->iterated_string = JS_UNDEFINED;
            p->u.regexp_string_iterator_data = it;
            snap_payload_ready(r);
            if (snap_get_value(r, &it->iterating_regexp) ||
                snap_get_value(r, &it->iterated_string))
                return -1;
        }
        break;
    case JS_CLASS_FOR_IN_ITERATOR:
        return snap_read_for_in_iterator(r, p);
    case JS_CLASS_PROXY:
        {
            JSProxyData *s;
            uint8_t is_func, is_revoked;
            if (snap_get_present(r, &present))
                return -1;
            if (!present)
                break;
            if (bc_get_u8(&r->bc, &is_func) || bc_get_u8(&r->bc, &is_revoked))
                return -1;
            s = js_malloc(r->ctx, sizeof(*s));
            if (!s)
                return -1;
            s->is_func = is_func != 0;
            s->is_revoked = is_revoked != 0;
            s->target = JS_UNDEFINED;
            s->handler = JS_UNDEFINED;
            p->u.proxy_data = s;
            snap_payload_ready(r);
            if (snap_get_value(r, &s->target) ||
                snap_get_value(r, &s->handler))
                return -1;
        }
        break;
    default:
        return 1;
    }
    return 0;
}

/*******************************************************************/
/* modules */

/* The pointers between modules (requested modules, cycle root, async
   parents) do not hold references, as in the engine. The temporary
   fields of the linking and evaluation DFS are not written: they are only
   meaningful while js_link_module() or js_evaluate_module() runs. */
static int snap_write_module(SnapWriter *w, JSModuleDef *m)
{
    BCWriterState *s = &w->bc;
    int i;

    if (m->status == JS_MODULE_STATUS_LINKING ||
        m->status == JS_MODULE_STATUS_EVALUATING) {
        JS_ThrowTypeError(w->ctx, "snapshot: module is being linked or evaluated");
        return -1;
    }
    if (bc_put_atom(s, m->module_name))
        return -1;

    bc_put_leb128(s, m->req_module_entries_count);
    for(i = 0; i < m->req_module_entries_count; i++) {
        JSReqModuleEntry *rme = &m->req_module_entries[i];
        if (bc_put_atom(s, rme->module_name) ||
            snap_put_ref(w, SNAP_CELL_MODULE, rme->module) ||
            snap_put_value(w, rme->attributes))
            return -1;
    }

    bc_put_leb128(s, m->export_entries_count);
    for(i = 0; i < m->export_entries_count; i++) {
        JSExportEntry *me = &m->export_entries[i];
        bc_put_u8(s, me->export_type);
        if (bc_put_atom(s, me->local_name) ||
            bc_put_atom(s, me->export_name))
            return -1;
        if (me->export_type == JS_EXPORT_TYPE_LOCAL) {
            bc_put_sleb128(s, me->u.local.var_idx);
            if (snap_put_ref(w, SNAP_CELL_VAR_REF, me->u.local.var_ref))
                return -1;
        } else {
            bc_put_leb128(s, me->u.req_module_idx);
        }
    }

    bc_put_leb128(s, m->star_export_entries_count);
    for(i = 0; i < m->star_export_entries_count; i++)
        bc_put_leb128(s, m->star_export_entries[i].req_module_idx);

    bc_put_leb128(s, m->import_entries_count);
    for(i = 0; i < m->import_entries_count; i++) {
        JSImportEntry *mi = &m->import_entries[i];
        bc_put_sleb128(s, mi->var_idx);
        bc_put_u8(s, mi->is_star);
        if (bc_put_atom(s, mi->import_name))
            return -1;
        bc_put_leb128(s, mi->req_module_idx);
    }

    if (snap_put_value(w, m->module_ns) ||
        snap_put_value(w, m->func_obj) ||
        snap_put_native(w, m->init_func))
        return -1;
    bc_put_u8(s, m->has_tla);
    bc_put_u8(s, m->resolved);
    bc_put_u8(s, m->func_created);
    bc_put_u8(s, m->status);

    bc_put_leb128(s, m->async_parent_modules_count);
    for(i = 0; i < m->async_parent_modules_count; i++) {
        if (snap_put_ref(w, SNAP_CELL_MODULE, m->async_parent_modules[i]))
            return -1;
    }
    bc_put_sleb128(s, m->pending_async_dependencies);
    bc_put_u8(s, m->async_evaluation);
    bc_put_u64(s, m->async_evaluation_timestamp);
    if (snap_put_ref(w, SNAP_CELL_MODULE, m->cycle_root) ||
        snap_put_value(w, m->promise) ||
        snap_put_values(w, m->resolving_funcs, 2))
        return -1;
    bc_put_u8(s, m->eval_has_exception);
    if (snap_put_value(w, m->eval_exception) ||
        snap_put_value(w, m->meta_obj) ||
        snap_put_value(w, m->private_value) ||
        snap_put_value(w, m->attributes))
        return -1;
    return 0;
}

/* the module stays consistent for js_free_module_def() at any time */
static int snap_read_module(SnapReader *r, JSModuleDef *m)
{
    JSContext *ctx = r->ctx;
    uint32_t i, n, idx;
    int32_t v;
    uint8_t has_tla, resolved, func_created, status, flag;
    uint64_t ts;
    void *init_func;

    if (bc_get_atom(&r->bc, &m->module_name))
        return -1;

    if (snap_get_count(r, &n))
        return -1;
    if (n > 0) {
        m->req_module_entries = js_mallocz(ctx, sizeof(m->req_module_entries[0]) * n);
        if (!m->req_module_entries)
            return -1;
        for(i = 0; i < n; i++)
            m->req_module_entries[i].attributes = JS_UNDEFINED;
        m->req_module_entries_size = n;
        m->req_module_entries_count = n;
    }
    for(i = 0; i < n; i++) {
        JSReqModuleEntry *rme = &m->req_module_entries[i];
        if (bc_get_atom(&r->bc, &rme->module_name) ||
            snap_get_ref(r, SNAP_CELL_MODULE, (void **)&rme->module) ||
            snap_get_value(r, &rme->attributes))
            return -1;
    }

    /* JS_EXPORT_TYPE_LOCAL entries without variable reference can be freed */
    if (snap_get_count(r, &n))
        return -1;
    if (n > 0) {
        m->export_entries = js_mallocz(ctx, sizeof(m->export_entries[0]) * n);
        if (!m->export_entries)
            return -1;
        m->export_entries_size = n;
        m->export_entries_count = n;
    }
    for(i = 0; i < n; i++) {
        JSExportEntry *me = &m->export_entries[i];
        if (bc_get_u8(&r->bc, &flag) ||
            bc_get_atom(&r->bc, &me->local_name) ||
            bc_get_atom(&r->bc, &me->export_name))
            return -1;
        if (flag == JS_EXPORT_TYPE_LOCAL) {
            JSVarRef *var_ref;
            if (bc_get_sleb128(&r->bc, &v) ||
                snap_get_ref(r, SNAP_CELL_VAR_REF, (void **)&var_ref))
                return -1;
            me->u.local.var_idx = v;
            if (var_ref)
                js_rc(var_ref)->ref_count++;
            me->u.local.var_ref = var_ref;
        } else if (flag == JS_EXPORT_TYPE_INDIRECT) {
            if (snap_get_leb(r, &idx))
                return -1;
            if (idx >= m->req_module_entries_count)
                return snap_error(r, "invalid module export");
            me->export_type = JS_EXPORT_TYPE_INDIRECT;
            me->u.req_module_idx = idx;
        } else {
            return snap_error(r, "invalid module export");
        }
    }

    if (snap_get_count(r, &n))
        return -1;
    if (n > 0) {
        m->star_export_entries = js_mallocz(ctx, sizeof(m->star_export_entries[0]) * n);
        if (!m->star_export_entries)
            return -1;
        m->star_export_entries_size = n;
        m->star_export_entries_count = n;
    }
    for(i = 0; i < n; i++) {
        if (snap_get_leb(r, &idx))
            return -1;
        if (idx >= m->req_module_entries_count)
            return snap_error(r, "invalid module star export");
        m->star_export_entries[i].req_module_idx = idx;
    }

    if (snap_get_count(r, &n))
        return -1;
    if (n > 0) {
        m->import_entries = js_mallocz(ctx, sizeof(m->import_entries[0]) * n);
        if (!m->import_entries)
            return -1;
        m->import_entries_size = n;
        m->import_entries_count = n;
    }
    for(i = 0; i < n; i++) {
        JSImportEntry *mi = &m->import_entries[i];
        if (bc_get_sleb128(&r->bc, &v) ||
            bc_get_u8(&r->bc, &flag) ||
            bc_get_atom(&r->bc, &mi->import_name) ||
            snap_get_leb(r, &idx))
            return -1;
        if (idx >= m->req_module_entries_count)
            return snap_error(r, "invalid module import");
        mi->var_idx = v;
        mi->is_star = flag != 0;
        mi->req_module_idx = idx;
    }

    if (snap_get_value(r, &m->module_ns) ||
        snap_get_value(r, &m->func_obj) ||
        snap_get_native(r, &init_func) ||
        bc_get_u8(&r->bc, &has_tla) ||
        bc_get_u8(&r->bc, &resolved) ||
        bc_get_u8(&r->bc, &func_created) ||
        bc_get_u8(&r->bc, &status))
        return -1;
    if (status > JS_MODULE_STATUS_EVALUATED ||
        status == JS_MODULE_STATUS_LINKING ||
        status == JS_MODULE_STATUS_EVALUATING)
        return snap_error(r, "invalid module status");
    m->init_func = init_func;
    m->has_tla = has_tla != 0;
    m->resolved = resolved != 0;
    m->func_created = func_created != 0;
    m->status = status;

    if (snap_get_count(r, &n))
        return -1;
    if (n > 0) {
        m->async_parent_modules = js_mallocz(ctx, sizeof(m->async_parent_modules[0]) * n);
        if (!m->async_parent_modules)
            return -1;
        m->async_parent_modules_size = n;
    }
    for(i = 0; i < n; i++) {
        JSModuleDef *m1;
        if (snap_get_ref(r, SNAP_CELL_MODULE, (void **)&m1))
            return -1;
        if (!m1)
            return snap_error(r, "invalid async parent module");
        m->async_parent_modules[m->async_parent_modules_count++] = m1;
    }
    if (bc_get_sleb128(&r->bc, &v) ||
        bc_get_u8(&r->bc, &flag) ||
        bc_get_u64(&r->bc, &ts) ||
        snap_get_ref(r, SNAP_CELL_MODULE, (void **)&m->cycle_root))
        return -1;
    m->pending_async_dependencies = v;
    m->async_evaluation = flag != 0;
    m->async_evaluation_timestamp = ts;
    if (snap_get_value(r, &m->promise) ||
        snap_get_values(r, m->resolving_funcs, 2) ||
        bc_get_u8(&r->bc, &flag))
        return -1;
    m->eval_has_exception = flag != 0;
    if (snap_get_value(r, &m->eval_exception) ||
        snap_get_value(r, &m->meta_obj) ||
        snap_get_value(r, &m->private_value) ||
        snap_get_value(r, &m->attributes))
        return -1;
    m->dfs_index = 0;
    m->dfs_ancestor_index = 0;
    m->stack_prev = NULL;
    return 0;
}

/* empty module record: link.prev/next are NULL until the body of its
   context adds it to the loaded module list */
static JSModuleDef *snap_new_module(SnapReader *r)
{
    JSModuleDef *m;

    m = js_mallocz(r->ctx, sizeof(*m));
    if (!m)
        return NULL;
    js_rc(m)->ref_count = 1;
    add_gc_object(r->rt, &m->header, JS_GC_OBJ_TYPE_MODULE);
    m->module_name = JS_ATOM_NULL;
    m->module_ns = JS_UNDEFINED;
    m->func_obj = JS_UNDEFINED;
    m->eval_exception = JS_UNDEFINED;
    m->meta_obj = JS_UNDEFINED;
    m->promise = JS_UNDEFINED;
    m->resolving_funcs[0] = JS_UNDEFINED;
    m->resolving_funcs[1] = JS_UNDEFINED;
    m->private_value = JS_UNDEFINED;
    m->attributes = JS_UNDEFINED;
    return m;
}
