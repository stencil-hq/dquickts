/*
 * dquickts Python C API: tracemalloc, port of CPython 3.15.2
 * Python/tracemalloc.c (PSF-2.0) for the runtime's allocators.  The
 * vendored Modules/_tracemalloc.c and Lib/tracemalloc.py run over the
 * functions defined here (capi/compat/pycore_tracemalloc.h).
 *
 * What is traced while tracing (domain 0, DEFAULT_DOMAIN, like CPython):
 *  - every block of the C-API runtime (qp_capi.rt) allocated with
 *    js_malloc_rt(), js_mallocz_rt(), js_calloc_rt() or js_realloc_rt():
 *    the Python objects, their payloads, the strings, the shapes... and the
 *    PyObject_Malloc blocks (QPCapiHead + object, traced at the head);
 *    JSRuntime.py_tm_tracing routes those functions here (one branch while
 *    not tracing, quickjs.c);
 *  - PyMem_Raw* and PyMem_* (qp_capi_tm_tracing, python/capi_mirror.c);
 *  - the blocks of PyTraceMalloc_Track() in their own domains (numpy).
 * A trace records the requested size and the traceback of the Python
 * frames of the allocating thread, most recent first: the frames of
 * rt->current_stack_frame running Python code (qp_fr_sf_func), the file
 * name of the bytecode and the line of the instruction being executed.
 * While tracing, the interpreter dispatch goes through the trace hook
 * (qp_trace_dispatch) which keeps sf->cur_pc current at each instruction,
 * so the line is the one of the allocating instruction.  Frames whose
 * first instruction has not run yet (argument binding, CPython's
 * incomplete frames) are skipped, like _PyFrame_GetFirstComplete().
 * A thread that does not hold the GIL (PyMem_Raw* in
 * Py_BEGIN_ALLOW_THREADS) gets the "<unknown>" traceback: its frames
 * live in the runtime that the GIL holder is running.
 *
 * Differences with CPython:
 *  - the blocks are the runtime's: a bytes object is a JSObject block plus
 *    its payload block (sizeof(QPBytesObject) + len + 1), not one block of
 *    sys.getsizeof() bytes; PyObject_Malloc blocks are traced at their head
 *    with the head included in the size;
 *  - get_object_traceback(obj) is the trace of the object's JS block (the
 *    JSObject, JSString... of its value), or of its C-API head for
 *    extension instances and C-primary objects; values without a block
 *    (small ints, floats, None...) have none;
 *  - file names are interned per atom (the bytecode's debug file name),
 *    the atoms being held by the table until the traces are cleared;
 *  - no PyRefTracer: the runtime has no free lists, every object is a new
 *    traced block.
 */

#define QP_CAPI_TM_DEFAULT_DOMAIN 0
#define QP_CAPI_TM_MAX_NFRAME UINT16_MAX
#define QP_CAPI_TM_TO_PTR(key) ((const void *)(uintptr_t)(key))
#define QP_CAPI_TM_FROM_PTR(key) ((uintptr_t)(key))

/* file name of the frames whose file name cannot be captured */
static const char qp_capi_tm_unknown_filename[] = "<unknown>";

typedef struct QPCapiTmFrame {
    const char *filename;       /* interned (qp_capi_tm.filenames) or qp_capi_tm_unknown_filename */
    unsigned int lineno;
} QPCapiTmFrame;

typedef struct QPCapiTmTraceback {
    Py_uhash_t hash;
    uint16_t nframe;            /* frames[] entries */
    uint16_t total_nframe;      /* frames of the stack (saturated at UINT16_MAX) */
    QPCapiTmFrame frames[];
} QPCapiTmTraceback;

#define QP_CAPI_TM_TRACEBACK_SIZE(n) \
    (sizeof(QPCapiTmTraceback) + sizeof(QPCapiTmFrame) * (size_t)(n))

/* trace of a memory block */
typedef struct QPCapiTmTrace {
    size_t size;
    QPCapiTmTraceback *traceback;
} QPCapiTmTrace;

/* _PyRuntime.tracemalloc.  Everything but max_nframe is protected by the
   lock, taken by threads that may not hold the GIL (PyMem_RawFree); the
   tracing flags are written under it, read without it by the allocators. */
static struct {
    int initialized;
    int tracing;
    int max_nframe;
    JSRuntime *rt;                      /* the traced runtime (qp_capi.rt) */
    _Py_hashtable_t *filenames;         /* JSAtom (held) -> interned UTF-8 file name (owned) */
    _Py_hashtable_t *tracebacks;        /* QPCapiTmTraceback* (owned) -> NULL */
    _Py_hashtable_t *traces;            /* DEFAULT_DOMAIN: block -> QPCapiTmTrace* (owned) */
    _Py_hashtable_t *domains;           /* domain -> traces table (owned) */
    QPCapiTmTraceback *traceback;       /* scratch traceback of max_nframe frames */
    QPCapiTmTraceback *empty_traceback; /* ("<unknown>", 0) */
    size_t traced_memory;
    size_t peak_traced_memory;
} qp_capi_tm = { .max_nframe = 1 };

static pthread_mutex_t qp_capi_tm_lock = PTHREAD_MUTEX_INITIALIZER;
#define QP_CAPI_TM_LOCK() pthread_mutex_lock(&qp_capi_tm_lock)
#define QP_CAPI_TM_UNLOCK() pthread_mutex_unlock(&qp_capi_tm_lock)

/* set while the thread allocates for tracemalloc itself (its Python
   results, clearing the tables): those blocks are not traced */
static _Thread_local int qp_capi_tm_reentrant;

/* fork() while another thread holds the lock: the child gets it unlocked */
static void qp_capi_tm_atfork_prepare(void)
{
    QP_CAPI_TM_LOCK();
}

static void qp_capi_tm_atfork_release(void)
{
    QP_CAPI_TM_UNLOCK();
}

/* ---- hash tables --------------------------------------------------------- */

static void *qp_capi_tm_raw_malloc(size_t size)
{
    return qp_sys_malloc(size);
}

static void qp_capi_tm_raw_free_block(void *ptr)
{
    qp_sys_free(ptr);
}

static _Py_hashtable_t *qp_capi_tm_hashtable_new(_Py_hashtable_hash_func hash_func,
                                                 _Py_hashtable_compare_func compare_func,
                                                 _Py_hashtable_destroy_func key_destroy_func,
                                                 _Py_hashtable_destroy_func value_destroy_func)
{
    _Py_hashtable_allocator_t alloc = { qp_capi_tm_raw_malloc, qp_capi_tm_raw_free_block };
    return _Py_hashtable_new_full(hash_func, compare_func, key_destroy_func,
                                  value_destroy_func, &alloc);
}

static Py_uhash_t qp_capi_tm_hash_uint(const void *key_raw)
{
    return (Py_uhash_t)(unsigned int)QP_CAPI_TM_FROM_PTR(key_raw);
}

static void qp_capi_tm_filename_atom_free(void *key)
{
    JS_FreeAtomRT(qp_capi_tm.rt, (JSAtom)QP_CAPI_TM_FROM_PTR(key));
}

static Py_uhash_t qp_capi_tm_hash_traceback(const void *key)
{
    return ((const QPCapiTmTraceback *)key)->hash;
}

static int qp_capi_tm_compare_traceback(const void *key1, const void *key2)
{
    const QPCapiTmTraceback *t1 = key1, *t2 = key2;
    int i;

    if (t1->nframe != t2->nframe || t1->total_nframe != t2->total_nframe)
        return 0;
    for (i = 0; i < t1->nframe; i++) {
        /* file names are interned: compared by pointer */
        if (t1->frames[i].lineno != t2->frames[i].lineno ||
            t1->frames[i].filename != t2->frames[i].filename)
            return 0;
    }
    return 1;
}

static _Py_hashtable_t *qp_capi_tm_create_traces_table(void)
{
    return qp_capi_tm_hashtable_new(_Py_hashtable_hash_ptr, _Py_hashtable_compare_direct,
                                    NULL, qp_capi_tm_raw_free_block);
}

static void qp_capi_tm_destroy_domain(void *value)
{
    _Py_hashtable_destroy((_Py_hashtable_t *)value);
}

static _Py_hashtable_t *qp_capi_tm_create_domains_table(void)
{
    return qp_capi_tm_hashtable_new(qp_capi_tm_hash_uint, _Py_hashtable_compare_direct,
                                    NULL, qp_capi_tm_destroy_domain);
}

/* ---- tracebacks ------------------------------------------------------------ */

/* UTF-8 (surrogatepass) copy of an atom's string, NULL on memory error. */
static char *qp_capi_tm_encode_filename(const JSString *p)
{
    uint32_t i, len = p->len;
    uint8_t *buf, *q;

    /* at most 3 bytes per UTF-16 unit (a pair takes 4) */
    buf = qp_sys_malloc((size_t)len * 3 + 1);
    if (!buf)
        return NULL;
    q = buf;
    for (i = 0; i < len; i++) {
        uint32_t c = p->is_wide_char ? p->u.str16[i] : p->u.str8[i];
        if (c >= 0xD800 && c < 0xDC00 && p->is_wide_char && i + 1 < len &&
            p->u.str16[i + 1] >= 0xDC00 && p->u.str16[i + 1] < 0xE000) {
            c = 0x10000 + ((c - 0xD800) << 10) + (p->u.str16[i + 1] - 0xDC00);
            i++;
        }
        if (c < 0x80) {
            *q++ = (uint8_t)c;
        } else if (c < 0x800) {
            *q++ = (uint8_t)(0xC0 | (c >> 6));
            *q++ = (uint8_t)(0x80 | (c & 0x3F));
        } else if (c < 0x10000) {
            *q++ = (uint8_t)(0xE0 | (c >> 12));
            *q++ = (uint8_t)(0x80 | ((c >> 6) & 0x3F));
            *q++ = (uint8_t)(0x80 | (c & 0x3F));
        } else {
            *q++ = (uint8_t)(0xF0 | (c >> 18));
            *q++ = (uint8_t)(0x80 | ((c >> 12) & 0x3F));
            *q++ = (uint8_t)(0x80 | ((c >> 6) & 0x3F));
            *q++ = (uint8_t)(0x80 | (c & 0x3F));
        }
    }
    *q = '\0';
    return (char *)buf;
}

/* The interned UTF-8 file name of atom, NULL on memory error.  The
   caller holds the lock (tracemalloc_intern_filename). */
static const char *qp_capi_tm_intern_filename(JSRuntime *rt, JSAtom atom)
{
    _Py_hashtable_entry_t *entry;
    char *filename;

    entry = _Py_hashtable_get_entry(qp_capi_tm.filenames, QP_CAPI_TM_TO_PTR(atom));
    if (entry)
        return entry->value;
    filename = qp_capi_tm_encode_filename(rt->atom_array[atom]);
    if (!filename)
        return NULL;
    if (_Py_hashtable_set(qp_capi_tm.filenames, QP_CAPI_TM_TO_PTR(atom), filename) < 0) {
        qp_sys_free(filename);
        return NULL;
    }
    /* the table holds the atom: its index is not reused for another name */
    JS_DupAtomRT(rt, atom);
    return filename;
}

/* tracemalloc_get_frame: file name and line of the Python frame sf */
static void qp_capi_tm_get_frame(JSRuntime *rt, JSStackFrame *sf, JSFunctionBytecode *b,
                                 QPCapiTmFrame *frame)
{
    int lineno = qp_fr_line_at(b, qp_fr_sf_pc(sf, b));
    JSAtom atom;

    frame->lineno = lineno < 0 ? 0 : (unsigned int)lineno;
    frame->filename = qp_capi_tm_unknown_filename;
    if (!b->has_debug)
        return;
    atom = b->debug.filename;
    if (atom == JS_ATOM_NULL || __JS_AtomIsTaggedInt(atom))
        return;
    {
        const char *filename = qp_capi_tm_intern_filename(rt, atom);
        if (filename)
            frame->filename = filename;
    }
}

static Py_uhash_t qp_capi_tm_traceback_hash(const QPCapiTmTraceback *traceback)
{
    /* code based on tuple_hash() of Objects/tupleobject.c */
    Py_uhash_t x, y;
    int len = traceback->nframe;
    Py_uhash_t mult = 1000003UL;  /* _PyHASH_MULTIPLIER */
    const QPCapiTmFrame *frame = traceback->frames;

    x = 0x345678UL;
    while (--len >= 0) {
        /* file names are interned: hash the pointer */
        y = (Py_uhash_t)Py_HashPointer(frame->filename);
        y ^= (Py_uhash_t)frame->lineno;
        frame++;
        x = (x ^ y) * mult;
        mult += (Py_uhash_t)(82520UL + len + len);
    }
    x ^= traceback->total_nframe;
    x += 97531UL;
    return x;
}

/* traceback_get_frames: the complete Python frames of the running thread,
   most recent first */
static void qp_capi_tm_traceback_get_frames(QPCapiTmTraceback *traceback, JSRuntime *rt)
{
    JSStackFrame *sf;

    for (sf = rt->current_stack_frame; sf; sf = sf->prev_frame) {
        JSObject *p = qp_fr_sf_func(sf);
        JSFunctionBytecode *b;
        if (!p)
            continue;
        b = p->u.func.function_bytecode;
        if (sf->cur_pc <= b->byte_code_buf ||
            sf->cur_pc > b->byte_code_buf + b->byte_code_len)
            continue;           /* not started: incomplete frame */
        if (traceback->nframe < qp_capi_tm.max_nframe) {
            qp_capi_tm_get_frame(rt, sf, b, &traceback->frames[traceback->nframe]);
            traceback->nframe++;
        }
        if (traceback->total_nframe < UINT16_MAX)
            traceback->total_nframe++;
    }
}

/* traceback_new: the interned traceback of the current frames, NULL on
   memory error.  The caller holds the lock. */
static QPCapiTmTraceback *qp_capi_tm_traceback_new(void)
{
    QPCapiTmTraceback *traceback, *copy;
    _Py_hashtable_entry_t *entry;
    size_t size;

    /* the frames are the GIL holder's (they live in the runtime) */
    if (!qp_capi_holds_gil())
        return qp_capi_tm.empty_traceback;
    traceback = qp_capi_tm.traceback;
    traceback->nframe = 0;
    traceback->total_nframe = 0;
    qp_capi_tm_traceback_get_frames(traceback, qp_capi_tm.rt);
    if (traceback->nframe == 0)
        return qp_capi_tm.empty_traceback;
    traceback->hash = qp_capi_tm_traceback_hash(traceback);

    entry = _Py_hashtable_get_entry(qp_capi_tm.tracebacks, traceback);
    if (entry)
        return (QPCapiTmTraceback *)entry->key;
    size = QP_CAPI_TM_TRACEBACK_SIZE(traceback->nframe);
    copy = qp_sys_malloc(size);
    if (!copy)
        return NULL;
    memcpy(copy, traceback, size);
    if (_Py_hashtable_set(qp_capi_tm.tracebacks, copy, NULL) < 0) {
        qp_sys_free(copy);
        return NULL;
    }
    return copy;
}

/* ---- traces ---------------------------------------------------------------- */

static _Py_hashtable_t *qp_capi_tm_get_traces_table(unsigned int domain)
{
    if (domain == QP_CAPI_TM_DEFAULT_DOMAIN)
        return qp_capi_tm.traces;
    return _Py_hashtable_get(qp_capi_tm.domains, QP_CAPI_TM_TO_PTR(domain));
}

/* tracemalloc_remove_trace_unlocked */
static void qp_capi_tm_remove_trace(unsigned int domain, uintptr_t ptr)
{
    _Py_hashtable_t *traces = qp_capi_tm_get_traces_table(domain);
    QPCapiTmTrace *trace;

    if (!traces)
        return;
    trace = _Py_hashtable_steal(traces, QP_CAPI_TM_TO_PTR(ptr));
    if (!trace)
        return;
    assert(qp_capi_tm.traced_memory >= trace->size);
    qp_capi_tm.traced_memory -= trace->size;
    qp_sys_free(trace);
}

/* tracemalloc_add_trace_unlocked: 0, or -1 on memory error */
static int qp_capi_tm_add_trace(unsigned int domain, uintptr_t ptr, size_t size)
{
    QPCapiTmTraceback *traceback = qp_capi_tm_traceback_new();
    _Py_hashtable_t *traces;
    QPCapiTmTrace *trace;

    if (!traceback)
        return -1;
    traces = qp_capi_tm_get_traces_table(domain);
    if (!traces) {
        traces = qp_capi_tm_create_traces_table();
        if (!traces)
            return -1;
        if (_Py_hashtable_set(qp_capi_tm.domains, QP_CAPI_TM_TO_PTR(domain), traces) < 0) {
            _Py_hashtable_destroy(traces);
            return -1;
        }
    }
    trace = _Py_hashtable_get(traces, QP_CAPI_TM_TO_PTR(ptr));
    if (trace) {
        /* the memory block is already tracked */
        assert(qp_capi_tm.traced_memory >= trace->size);
        qp_capi_tm.traced_memory -= trace->size;
        trace->size = size;
        trace->traceback = traceback;
    } else {
        trace = qp_sys_malloc(sizeof(*trace));
        if (!trace)
            return -1;
        trace->size = size;
        trace->traceback = traceback;
        if (_Py_hashtable_set(traces, QP_CAPI_TM_TO_PTR(ptr), trace) < 0) {
            qp_sys_free(trace);
            return -1;
        }
    }
    qp_capi_tm.traced_memory += size;
    if (qp_capi_tm.traced_memory > qp_capi_tm.peak_traced_memory)
        qp_capi_tm.peak_traced_memory = qp_capi_tm.traced_memory;
    return 0;
}

/* A new block: trace it, 0 or -1 if the trace could not be stored. */
static int qp_capi_tm_alloc_traced(void *ptr, size_t size)
{
    int r = 0;

    QP_CAPI_TM_LOCK();
    /* else: stopped by another thread meanwhile (gh-128679) */
    if (qp_capi_tm.tracing)
        r = qp_capi_tm_add_trace(QP_CAPI_TM_DEFAULT_DOMAIN, (uintptr_t)ptr, size);
    QP_CAPI_TM_UNLOCK();
    return r;
}

/* ptr is resized to ptr2 (tracemalloc_realloc) */
static void qp_capi_tm_realloc_traced(void *ptr, void *ptr2, size_t new_size)
{
    QP_CAPI_TM_LOCK();
    if (qp_capi_tm.tracing) {
        if (ptr2 != ptr)
            qp_capi_tm_remove_trace(QP_CAPI_TM_DEFAULT_DOMAIN, (uintptr_t)ptr);
        /* a trace entry was just released: the table has room; if not, the
           block stays untraced (CPython: fatal error) */
        (void)qp_capi_tm_add_trace(QP_CAPI_TM_DEFAULT_DOMAIN, (uintptr_t)ptr2, new_size);
    }
    QP_CAPI_TM_UNLOCK();
}

/* ptr is released: forget its trace first, so that another thread
   reusing the address keeps its own trace */
static void qp_capi_tm_free_traced(void *ptr)
{
    QP_CAPI_TM_LOCK();
    if (qp_capi_tm.tracing)
        qp_capi_tm_remove_trace(QP_CAPI_TM_DEFAULT_DOMAIN, (uintptr_t)ptr);
    QP_CAPI_TM_UNLOCK();
}

/* ---- allocator hooks --------------------------------------------------------- */

/* js_malloc_rt / js_mallocz_rt / js_calloc_rt (always under the GIL) */
static no_inline void *qp_capi_tm_js_alloc(JSRuntime *rt, size_t count, size_t size, BOOL zero)
{
    void *ptr = zero ? __js_calloc(&rt->malloc_ctx, count, size)
                     : __js_malloc(&rt->malloc_ctx, size);

    if (unlikely(!ptr) || qp_capi_tm_reentrant)
        return ptr;
    if (qp_capi_tm_alloc_traced(ptr, count * size) < 0) {
        __js_free(&rt->malloc_ctx, ptr);
        return NULL;
    }
    return ptr;
}

static no_inline void *qp_capi_tm_js_realloc(JSRuntime *rt, void *ptr, size_t size)
{
    void *ptr2;

    if (qp_capi_tm_reentrant)
        return __js_realloc(&rt->malloc_ctx, ptr, size);
    if (!ptr)
        return qp_capi_tm_js_alloc(rt, 1, size, FALSE);
    if (size == 0) {
        /* __js_realloc() frees the block */
        qp_capi_tm_free_traced(ptr);
        return __js_realloc(&rt->malloc_ctx, ptr, size);
    }
    ptr2 = __js_realloc(&rt->malloc_ctx, ptr, size);
    if (ptr2)
        qp_capi_tm_realloc_traced(ptr, ptr2, size);
    return ptr2;
}

static no_inline void qp_capi_tm_js_free(JSRuntime *rt, void *ptr)
{
    if (ptr && !qp_capi_tm_reentrant)
        qp_capi_tm_free_traced(ptr);
    __js_free(&rt->malloc_ctx, ptr);
}

/* PyMem_RawMalloc / PyMem_RawCalloc (with or without the GIL) */
static void *qp_capi_tm_raw_alloc(size_t nelem, size_t elsize, BOOL zero)
{
    size_t size = nelem * elsize;   /* checked by the caller */
    void *ptr;

    if (zero)
        ptr = size ? qp_sys_calloc(nelem, elsize) : qp_sys_calloc(1, 1);
    else
        ptr = qp_sys_malloc(size ? size : 1);
    if (unlikely(!ptr) || qp_capi_tm_reentrant)
        return ptr;
    if (qp_capi_tm_alloc_traced(ptr, size) < 0) {
        qp_sys_free(ptr);
        return NULL;
    }
    return ptr;
}

static void *qp_capi_tm_raw_realloc(void *ptr, size_t new_size)
{
    void *ptr2;

    if (!ptr)
        return qp_capi_tm_raw_alloc(1, new_size, FALSE);
    ptr2 = qp_sys_realloc(ptr, new_size ? new_size : 1);
    if (ptr2 && !qp_capi_tm_reentrant)
        qp_capi_tm_realloc_traced(ptr, ptr2, new_size);
    return ptr2;
}

static void qp_capi_tm_raw_free(void *ptr)
{
    if (ptr && !qp_capi_tm_reentrant)
        qp_capi_tm_free_traced(ptr);
    qp_sys_free(ptr);
}

/* ---- state ------------------------------------------------------------------ */

/* tracemalloc_clear_traces_unlocked */
static void qp_capi_tm_clear_traces_unlocked(void)
{
    qp_capi_tm_reentrant = 1;   /* the atoms of the file names are freed */
    _Py_hashtable_clear(qp_capi_tm.traces);
    _Py_hashtable_clear(qp_capi_tm.domains);
    _Py_hashtable_clear(qp_capi_tm.tracebacks);
    _Py_hashtable_clear(qp_capi_tm.filenames);
    qp_capi_tm.traced_memory = 0;
    qp_capi_tm.peak_traced_memory = 0;
    qp_capi_tm_reentrant = 0;
}

/* _PyTraceMalloc_Init (on the first start): 0, or -1 on memory error */
static int qp_capi_tm_init(void)
{
    QPCapiTmTraceback *empty;

    if (qp_capi_tm.initialized)
        return 0;
    qp_capi_tm.filenames = qp_capi_tm_hashtable_new(_Py_hashtable_hash_ptr,
                                                    _Py_hashtable_compare_direct,
                                                    qp_capi_tm_filename_atom_free,
                                                    qp_capi_tm_raw_free_block);
    qp_capi_tm.tracebacks = qp_capi_tm_hashtable_new(qp_capi_tm_hash_traceback,
                                                     qp_capi_tm_compare_traceback,
                                                     qp_capi_tm_raw_free_block, NULL);
    qp_capi_tm.traces = qp_capi_tm_create_traces_table();
    qp_capi_tm.domains = qp_capi_tm_create_domains_table();
    empty = qp_sys_malloc(QP_CAPI_TM_TRACEBACK_SIZE(1));
    if (!qp_capi_tm.filenames || !qp_capi_tm.tracebacks || !qp_capi_tm.traces ||
        !qp_capi_tm.domains || !empty) {
        if (qp_capi_tm.filenames)
            _Py_hashtable_destroy(qp_capi_tm.filenames);
        if (qp_capi_tm.tracebacks)
            _Py_hashtable_destroy(qp_capi_tm.tracebacks);
        if (qp_capi_tm.traces)
            _Py_hashtable_destroy(qp_capi_tm.traces);
        if (qp_capi_tm.domains)
            _Py_hashtable_destroy(qp_capi_tm.domains);
        qp_sys_free(empty);
        qp_capi_tm.filenames = qp_capi_tm.tracebacks = NULL;
        qp_capi_tm.traces = qp_capi_tm.domains = NULL;
        return -1;
    }
    empty->nframe = 1;
    empty->total_nframe = 1;
    empty->frames[0].filename = qp_capi_tm_unknown_filename;
    empty->frames[0].lineno = 0;
    empty->hash = qp_capi_tm_traceback_hash(empty);
    qp_capi_tm.empty_traceback = empty;
    pthread_atfork(qp_capi_tm_atfork_prepare, qp_capi_tm_atfork_release,
                   qp_capi_tm_atfork_release);
    qp_capi_tm.initialized = 1;
    return 0;
}

int _PyTraceMalloc_IsTracing(void)
{
    int tracing;

    QP_CAPI_TM_LOCK();
    tracing = qp_capi_tm.tracing;
    QP_CAPI_TM_UNLOCK();
    return tracing;
}

void _PyTraceMalloc_ClearTraces(void)
{
    QP_CAPI_TM_LOCK();
    if (qp_capi_tm.tracing)
        qp_capi_tm_clear_traces_unlocked();
    QP_CAPI_TM_UNLOCK();
}

int _PyTraceMalloc_Start(int max_nframe)
{
    JSContext *ctx = qp_capi.ctx;
    JSRuntime *rt = ctx->rt;
    QPCapiTmTraceback *traceback;

    if (max_nframe < 1 || max_nframe > QP_CAPI_TM_MAX_NFRAME) {
        PyErr_Format(PyExc_ValueError,
                     "the number of frames must be in range [1; %i]",
                     QP_CAPI_TM_MAX_NFRAME);
        return -1;
    }
    if (_PyTraceMalloc_IsTracing()) {
        /* hooks already installed: do nothing */
        return 0;
    }
    if (qp_capi_tm_init() < 0) {
        PyErr_NoMemory();
        return -1;
    }
    /* the line of an allocation is the one of the instruction running
       (sf->cur_pc kept by the trace hook) */
    if (qp_trace_dispatch_export(ctx) < 0)
        return -1;
    qp_capi_tm.max_nframe = max_nframe;
    /* a buffer to store a new traceback */
    traceback = qp_sys_malloc(QP_CAPI_TM_TRACEBACK_SIZE(max_nframe));
    if (!traceback) {
        PyErr_NoMemory();
        return -1;
    }
    qp_trace_dispatch(1);

    /* everything is ready: start tracing the allocations */
    QP_CAPI_TM_LOCK();
    qp_capi_tm.traceback = traceback;
    qp_capi_tm.rt = rt;
    rt->py_tm_tracing = TRUE;
    __atomic_store_n(&qp_capi_tm_tracing, 1, __ATOMIC_RELAXED);
    qp_capi_tm.tracing = 1;
    QP_CAPI_TM_UNLOCK();
    return 0;
}

void _PyTraceMalloc_Stop(void)
{
    QP_CAPI_TM_LOCK();
    if (!qp_capi_tm.tracing) {
        QP_CAPI_TM_UNLOCK();
        return;
    }
    /* stop tracing the allocations */
    qp_capi_tm.tracing = 0;
    __atomic_store_n(&qp_capi_tm_tracing, 0, __ATOMIC_RELAXED);
    qp_capi_tm.rt->py_tm_tracing = FALSE;
    qp_capi_tm_clear_traces_unlocked();
    qp_sys_free(qp_capi_tm.traceback);
    qp_capi_tm.traceback = NULL;
    QP_CAPI_TM_UNLOCK();
    qp_trace_dispatch(0);
}

static void qp_capi_tm_context_free(JSContext *ctx)
{
    (void)ctx;
    _PyTraceMalloc_Stop();
}

/* ---- Python results ------------------------------------------------------- */

/* a JS value stored in the temporary intern tables (owned) */
typedef struct QPCapiTmBox {
    JSValue v;
} QPCapiTmBox;

static void qp_capi_tm_box_free(void *value)
{
    QPCapiTmBox *box = value;
    JS_FreeValue(qp_capi.ctx, box->v);
    qp_sys_free(box);
}

/* table key -> JSValue (new reference kept by intern), or exception */
static JSValue qp_capi_tm_intern_value(JSContext *ctx, _Py_hashtable_t *intern,
                                       const void *key, JSValue v)
{
    QPCapiTmBox *box;

    if (JS_IsException(v) || !intern)
        return v;
    box = qp_sys_malloc(sizeof(*box));
    if (!box || _Py_hashtable_set(intern, key, box) < 0) {
        qp_sys_free(box);
        JS_FreeValue(ctx, v);
        return qp_raise_memory_error(ctx);
    }
    box->v = JS_DupValue(ctx, v);
    return v;
}

/* filename_to_pyobject */
static JSValue qp_capi_tm_filename_value(JSContext *ctx, const char *filename,
                                         _Py_hashtable_t *intern_filenames)
{
    JSValue v;

    if (intern_filenames) {
        QPCapiTmBox *box = _Py_hashtable_get(intern_filenames, filename);
        if (box)
            return JS_DupValue(ctx, box->v);
    }
    if (filename == qp_capi_tm_unknown_filename)
        v = qp_str_from_cstr(ctx, filename);
    else
        v = qp_str_decode(ctx, filename, (Py_ssize_t)strlen(filename), "utf-8", "surrogatepass");
    return qp_capi_tm_intern_value(ctx, intern_filenames, filename, v);
}

/* traceback_to_pyobject: tuple of (filename, lineno) tuples */
static JSValue qp_capi_tm_traceback_value(JSContext *ctx, const QPCapiTmTraceback *traceback,
                                          _Py_hashtable_t *intern_table,
                                          _Py_hashtable_t *intern_filenames)
{
    JSValue frames, *items;
    int i;

    if (intern_table) {
        QPCapiTmBox *box = _Py_hashtable_get(intern_table, traceback);
        if (box)
            return JS_DupValue(ctx, box->v);
    }
    frames = qp_tuple_new(ctx, traceback->nframe);
    if (JS_IsException(frames))
        return frames;
    items = QP_TUPLE_ITEMS(frames);
    for (i = 0; i < traceback->nframe; i++) {
        JSValue pair[2];
        pair[0] = qp_capi_tm_filename_value(ctx, traceback->frames[i].filename, intern_filenames);
        if (JS_IsException(pair[0]))
            goto fail;
        pair[1] = qp_long_from_uint64(ctx, traceback->frames[i].lineno);
        if (JS_IsException(pair[1])) {
            JS_FreeValue(ctx, pair[0]);
            goto fail;
        }
        items[i] = qp_tuple_new_steal(ctx, 2, pair);
        if (JS_IsException(items[i])) {
            items[i] = JS_NULL;
            goto fail;
        }
    }
    return qp_capi_tm_intern_value(ctx, intern_table, traceback, frames);
 fail:
    JS_FreeValue(ctx, frames);
    return JS_EXCEPTION;
}

/* trace_to_pyobject: (domain, size, traceback, total_nframe) */
static JSValue qp_capi_tm_trace_value(JSContext *ctx, unsigned int domain,
                                      const QPCapiTmTrace *trace,
                                      _Py_hashtable_t *intern_tracebacks,
                                      _Py_hashtable_t *intern_filenames)
{
    JSValue items[4];
    int i;

    items[0] = qp_long_from_uint64(ctx, domain);
    items[1] = qp_long_from_uint64(ctx, trace->size);
    items[2] = qp_capi_tm_traceback_value(ctx, trace->traceback, intern_tracebacks,
                                          intern_filenames);
    items[3] = qp_long_from_uint64(ctx, trace->traceback->total_nframe);
    for (i = 0; i < 4; i++) {
        if (JS_IsException(items[i])) {
            int j;
            for (j = 0; j < 4; j++)
                JS_FreeValue(ctx, items[j]);
            return JS_EXCEPTION;
        }
    }
    return qp_tuple_new_steal(ctx, 4, items);
}

typedef struct QPCapiTmGetTraces {
    JSContext *ctx;
    _Py_hashtable_t *tracebacks;    /* traceback -> frames tuple */
    _Py_hashtable_t *filenames;     /* interned file name -> str */
    JSValue list;
    unsigned int domain;
} QPCapiTmGetTraces;

static int qp_capi_tm_get_traces_fill(_Py_hashtable_t *traces, const void *key,
                                      const void *value, void *user_data)
{
    QPCapiTmGetTraces *gt = user_data;
    JSValue tuple;
    int r;

    (void)traces;
    (void)key;
    tuple = qp_capi_tm_trace_value(gt->ctx, gt->domain, value, gt->tracebacks, gt->filenames);
    if (JS_IsException(tuple))
        return 1;
    r = qp_list_append(gt->ctx, gt->list, tuple);
    JS_FreeValue(gt->ctx, tuple);
    return r < 0;
}

static int qp_capi_tm_get_traces_domain(_Py_hashtable_t *domains, const void *key,
                                        const void *value, void *user_data)
{
    QPCapiTmGetTraces *gt = user_data;

    (void)domains;
    gt->domain = (unsigned int)QP_CAPI_TM_FROM_PTR(key);
    return _Py_hashtable_foreach((_Py_hashtable_t *)value, qp_capi_tm_get_traces_fill, gt);
}

/* Python objects built under the lock: no collection meanwhile, whose
   finalizers could run Python code (and release the GIL to a thread that
   would wait for the lock).  The allocations are not traced. */
static size_t qp_capi_tm_results_begin(JSRuntime *rt)
{
    size_t threshold = rt->malloc_gc_threshold;
    rt->malloc_gc_threshold = SIZE_MAX;
    qp_capi_tm_reentrant = 1;
    return threshold;
}

static void qp_capi_tm_results_end(JSRuntime *rt, size_t threshold)
{
    qp_capi_tm_reentrant = 0;
    rt->malloc_gc_threshold = threshold;
}

PyObject *_PyTraceMalloc_GetTraces(void)
{
    JSContext *ctx = qp_capi.ctx;
    QPCapiTmGetTraces gt;
    size_t threshold;
    int err;

    gt.ctx = ctx;
    gt.domain = QP_CAPI_TM_DEFAULT_DOMAIN;
    gt.tracebacks = NULL;
    gt.filenames = NULL;
    QP_CAPI_TM_LOCK();
    threshold = qp_capi_tm_results_begin(ctx->rt);
    gt.list = qp_list_new(ctx, 0);
    if (JS_IsException(gt.list) || !qp_capi_tm.tracing)
        goto done;
    /* temporary tables sharing the traceback tuples and the file names */
    gt.tracebacks = qp_capi_tm_hashtable_new(_Py_hashtable_hash_ptr, _Py_hashtable_compare_direct,
                                             NULL, qp_capi_tm_box_free);
    gt.filenames = qp_capi_tm_hashtable_new(_Py_hashtable_hash_ptr, _Py_hashtable_compare_direct,
                                            NULL, qp_capi_tm_box_free);
    if (!gt.tracebacks || !gt.filenames) {
        qp_raise_memory_error(ctx);
        err = 1;
    } else {
        /* the allocations of the results are not traced: the tables do
           not change while they are walked */
        err = _Py_hashtable_foreach(qp_capi_tm.traces, qp_capi_tm_get_traces_fill, &gt);
        if (!err)
            err = _Py_hashtable_foreach(qp_capi_tm.domains, qp_capi_tm_get_traces_domain, &gt);
    }
    if (err) {
        JS_FreeValue(ctx, gt.list);
        gt.list = JS_EXCEPTION;
    }
 done:
    if (gt.tracebacks)
        _Py_hashtable_destroy(gt.tracebacks);
    if (gt.filenames)
        _Py_hashtable_destroy(gt.filenames);
    qp_capi_tm_results_end(ctx->rt, threshold);
    QP_CAPI_TM_UNLOCK();
    return qp_capi_from_js_result(ctx, gt.list);
}

/* tracemalloc_get_traceback_unlocked */
static QPCapiTmTraceback *qp_capi_tm_get_traceback_unlocked(unsigned int domain, uintptr_t ptr)
{
    _Py_hashtable_t *traces;
    QPCapiTmTrace *trace;

    if (!qp_capi_tm.tracing)
        return NULL;
    traces = qp_capi_tm_get_traces_table(domain);
    if (!traces)
        return NULL;
    trace = _Py_hashtable_get(traces, QP_CAPI_TM_TO_PTR(ptr));
    return trace ? trace->traceback : NULL;
}

/* The frames of the first traced block of ptrs, or None. */
static PyObject *qp_capi_tm_get_traceback(unsigned int domain, const uintptr_t *ptrs, int n)
{
    JSContext *ctx = qp_capi.ctx;
    QPCapiTmTraceback *traceback = NULL;
    JSValue r;
    size_t threshold;
    int i;

    QP_CAPI_TM_LOCK();
    for (i = 0; i < n && !traceback; i++)
        traceback = qp_capi_tm_get_traceback_unlocked(domain, ptrs[i]);
    if (traceback) {
        threshold = qp_capi_tm_results_begin(ctx->rt);
        r = qp_capi_tm_traceback_value(ctx, traceback, NULL, NULL);
        qp_capi_tm_results_end(ctx->rt, threshold);
    } else {
        r = JS_UNDEFINED;   /* None */
    }
    QP_CAPI_TM_UNLOCK();
    return qp_capi_from_js_result(ctx, r);
}

PyObject *_PyTraceMalloc_GetTraceback(unsigned int domain, uintptr_t ptr)
{
    return qp_capi_tm_get_traceback(domain, &ptr, 1);
}

/* The block of obj: the C-API head of an extension instance or of a
   C-primary object (not crossed yet), else the block of its JS value (the
   head of a mirror is only the C view of the object, made when it first
   crossed to C).  Statically allocated objects have none. */
PyObject *_PyTraceMalloc_GetObjectTraceback(PyObject *obj)
{
    uintptr_t ptrs[2];
    int n = 0;

    if (QP_CAPI_HAS_HEAD(obj)) {
        QPCapiHead *h = QP_CAPI_HEAD(obj);
        JSValue v = h->v;
        if (h->kind == QP_CAPI_K_NATIVE || QP_IS_NULL(v))
            ptrs[n++] = (uintptr_t)h;
        if (!QP_IS_NULL(v) && JS_VALUE_HAS_REF_COUNT(v))
            ptrs[n++] = (uintptr_t)JS_VALUE_GET_PTR(v);
    }
    return qp_capi_tm_get_traceback(QP_CAPI_TM_DEFAULT_DOMAIN, ptrs, n);
}

int _PyTraceMalloc_GetTracebackLimit(void)
{
    return qp_capi_tm.max_nframe;
}

static int qp_capi_tm_get_memory_cb(_Py_hashtable_t *domains, const void *key,
                                    const void *value, void *user_data)
{
    (void)domains;
    (void)key;
    *(size_t *)user_data += _Py_hashtable_size(value);
    return 0;
}

size_t _PyTraceMalloc_GetMemory(void)
{
    size_t size = 0;

    QP_CAPI_TM_LOCK();
    if (qp_capi_tm.tracing) {
        size = _Py_hashtable_size(qp_capi_tm.tracebacks);
        size += _Py_hashtable_size(qp_capi_tm.filenames);
        size += _Py_hashtable_size(qp_capi_tm.traces);
        _Py_hashtable_foreach(qp_capi_tm.domains, qp_capi_tm_get_memory_cb, &size);
    }
    QP_CAPI_TM_UNLOCK();
    return size;
}

PyObject *_PyTraceMalloc_GetTracedMemory(void)
{
    Py_ssize_t traced = 0, peak = 0;

    QP_CAPI_TM_LOCK();
    if (qp_capi_tm.tracing) {
        traced = (Py_ssize_t)qp_capi_tm.traced_memory;
        peak = (Py_ssize_t)qp_capi_tm.peak_traced_memory;
    }
    QP_CAPI_TM_UNLOCK();
    return Py_BuildValue("nn", traced, peak);
}

void _PyTraceMalloc_ResetPeak(void)
{
    QP_CAPI_TM_LOCK();
    if (qp_capi_tm.tracing)
        qp_capi_tm.peak_traced_memory = qp_capi_tm.traced_memory;
    QP_CAPI_TM_UNLOCK();
}

/* Include/cpython/tracemalloc.h */
int PyTraceMalloc_Track(unsigned int domain, uintptr_t ptr, size_t size)
{
    int result;

    if (!__atomic_load_n(&qp_capi_tm_tracing, __ATOMIC_RELAXED))
        return -2;
    QP_CAPI_TM_LOCK();
    if (qp_capi_tm.tracing)
        result = qp_capi_tm_add_trace(domain, ptr, size);
    else
        result = -2;
    QP_CAPI_TM_UNLOCK();
    return result;
}

int PyTraceMalloc_Untrack(unsigned int domain, uintptr_t ptr)
{
    int result;

    if (!__atomic_load_n(&qp_capi_tm_tracing, __ATOMIC_RELAXED))
        return -2;
    QP_CAPI_TM_LOCK();
    if (qp_capi_tm.tracing) {
        qp_capi_tm_remove_trace(domain, ptr);
        result = 0;
    } else {
        result = -2;
    }
    QP_CAPI_TM_UNLOCK();
    return result;
}

#undef QP_CAPI_TM_DEFAULT_DOMAIN
#undef QP_CAPI_TM_MAX_NFRAME
#undef QP_CAPI_TM_TO_PTR
#undef QP_CAPI_TM_FROM_PTR
#undef QP_CAPI_TM_TRACEBACK_SIZE
#undef QP_CAPI_TM_LOCK
#undef QP_CAPI_TM_UNLOCK
