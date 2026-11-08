/*
 * sort.c - CPython's list.sort (adaptive, stable, natural mergesort with
 * powersort run merging) over JSValue arrays.
 *
 * Derived from CPython Objects/listobject.c (PSF-2.0), v3.15.2.
 * See cpython/Objects/listsort.txt for the algorithm's design notes.
 *
 * The port is line-for-line: binarysort, count_run, gallop_left/right,
 * merge_lo/merge_hi, merge_at, powerloop, found_new_run, merge_force_collapse
 * and minrun_next keep CPython's control flow, so the sequence of `lt` calls
 * (and therefore every side effect a Python `__lt__` can observe) is the one
 * CPython makes.  Differences:
 *  - PyObject* slots are JSValue slots; ms->key_compare(X, Y, ms) is the
 *    caller's lt(ctx, X, Y, opaque);
 *  - temp memory comes from js_malloc/js_free on ctx (an allocation failure
 *    leaves an "out of memory" exception pending on ctx, where CPython raises
 *    MemoryError);
 *  - helpers carry a qp_ts_ prefix (one translation unit with quickjs.c and
 *    with whatever later ports the rest of listobject.c);
 *  - list bookkeeping (key calls, reverse, "list modified during sort") is
 *    the caller's job, see sort.h.
 */

/* A sortslice contains a pointer to an array of keys and a pointer to
 * an array of corresponding values.  In other words, keys[i]
 * corresponds with values[i].  If values == NULL, then the keys are
 * also the values.
 *
 * Several convenience routines are provided here, so that keys and
 * values are always moved in sync.
 */
typedef struct {
    JSValue *keys;
    JSValue *values;
} qp_ts_sortslice;

/* Reverse a slice of an array in place, from lo up to (exclusive) hi. */
static void qp_ts_reverse_slice(JSValue *lo, JSValue *hi)
{
    assert(lo && hi);

    --hi;
    while (lo < hi) {
        JSValue t = *lo;
        *lo = *hi;
        *hi = t;
        ++lo;
        --hi;
    }
}

static void qp_sort_reverse(JSValue *a, Py_ssize_t n)
{
    if (n > 1)
        qp_ts_reverse_slice(a, a + n);
}

static inline void qp_ts_sortslice_copy(qp_ts_sortslice *s1, Py_ssize_t i,
                                        qp_ts_sortslice *s2, Py_ssize_t j)
{
    s1->keys[i] = s2->keys[j];
    if (s1->values != NULL)
        s1->values[i] = s2->values[j];
}

static inline void qp_ts_sortslice_copy_incr(qp_ts_sortslice *dst,
                                             qp_ts_sortslice *src)
{
    *dst->keys++ = *src->keys++;
    if (dst->values != NULL)
        *dst->values++ = *src->values++;
}

static inline void qp_ts_sortslice_copy_decr(qp_ts_sortslice *dst,
                                             qp_ts_sortslice *src)
{
    *dst->keys-- = *src->keys--;
    if (dst->values != NULL)
        *dst->values-- = *src->values--;
}

static inline void qp_ts_sortslice_memcpy(qp_ts_sortslice *s1, Py_ssize_t i,
                                          qp_ts_sortslice *s2, Py_ssize_t j,
                                          Py_ssize_t n)
{
    memcpy(&s1->keys[i], &s2->keys[j], sizeof(JSValue) * n);
    if (s1->values != NULL)
        memcpy(&s1->values[i], &s2->values[j], sizeof(JSValue) * n);
}

static inline void qp_ts_sortslice_memmove(qp_ts_sortslice *s1, Py_ssize_t i,
                                           qp_ts_sortslice *s2, Py_ssize_t j,
                                           Py_ssize_t n)
{
    memmove(&s1->keys[i], &s2->keys[j], sizeof(JSValue) * n);
    if (s1->values != NULL)
        memmove(&s1->values[i], &s2->values[j], sizeof(JSValue) * n);
}

static inline void qp_ts_sortslice_advance(qp_ts_sortslice *slice, Py_ssize_t n)
{
    slice->keys += n;
    if (slice->values != NULL)
        slice->values += n;
}

/* Comparison function: the caller's strict less-than.
 * Returns -1 on error, 1 if x < y, 0 if x >= y.
 */
#define ISLT(X, Y) (ms->lt(ms->ctx, (X), (Y), ms->opaque))

/* Compare X to Y via "<".  Goto "fail" if the comparison raises an
   error.  Else "k" is set to true iff X<Y, and an "if (k)" block is
   started.  It makes more sense in context <wink>.  X and Y are JSValues.
*/
#define IFLT(X, Y) if ((k = ISLT(X, Y)) < 0) goto fail;  \
           if (k)

/* The maximum number of entries in a MergeState's pending-runs stack.
 * For a list with n elements, this needs at most floor(log2(n)) + 1 entries
 * even if we didn't force runs to a minimal length.  So the number of bits
 * in a Py_ssize_t is plenty large enough for all cases.
 */
#define MAX_MERGE_PENDING (sizeof(size_t) * 8)

/* When we get into galloping mode, we stay there until both runs win less
 * often than MIN_GALLOP consecutive times.  See listsort.txt for more info.
 */
#define MIN_GALLOP 7

/* Avoid malloc for small temp arrays. */
#define MERGESTATE_TEMP_SIZE 256

/* The largest value of minrun. This must be a power of 2, and >= 1 */
#define MAX_MINRUN 64
#if ((MAX_MINRUN) < 1) || ((MAX_MINRUN) & ((MAX_MINRUN) - 1))
#error "MAX_MINRUN must be a power of 2, and >= 1"
#endif

/* One MergeState exists on the stack per invocation of mergesort.  It's just
 * a convenient way to pass state around among the helper functions.
 */
struct qp_ts_slice {
    qp_ts_sortslice base;
    Py_ssize_t len;   /* length of run */
    int power; /* node "level" for powersort merge strategy */
};

typedef struct qp_ts_MergeState {
    /* This controls when we get *into* galloping mode.  It's initialized
     * to MIN_GALLOP.  merge_lo and merge_hi tend to nudge it higher for
     * random data, and lower for highly structured data.
     */
    Py_ssize_t min_gallop;

    Py_ssize_t listlen;     /* len(input_list) - read only */
    JSValue *basekeys;      /* base address of keys array - read only */

    /* 'a' is temp storage to help with merges.  It contains room for
     * alloced entries.
     */
    qp_ts_sortslice a;      /* may point to temparray below */
    Py_ssize_t alloced;

    /* A stack of n pending runs yet to be merged.  Run #i starts at
     * address base[i] and extends for len[i] elements.  It's always
     * true (so long as the indices are in bounds) that
     *
     *     pending[i].base + pending[i].len == pending[i+1].base
     *
     * so we could cut the storage for this, but it's a minor amount,
     * and keeping all the info explicit simplifies the code.
     */
    int n;
    struct qp_ts_slice pending[MAX_MERGE_PENDING];

    /* 'a' points to this when possible, rather than muck with malloc. */
    JSValue temparray[MERGESTATE_TEMP_SIZE];

    /* The comparison: lt(ctx, x, y, opaque) (CPython: key_compare). */
    JSContext *ctx;
    qp_sort_lt_fn *lt;
    void *opaque;

    /* Variables used for minrun computation. The "ideal" minrun length is
     * the infinite precision listlen / 2**e. See listsort.txt.
     */
    Py_ssize_t mr_current, mr_e, mr_mask;
} qp_ts_MergeState;

/* binarysort is the best method for sorting small arrays: it does few
   compares, but can do data movement quadratic in the number of elements.
   ss->keys is viewed as an array of n keys, a[:n]. a[:ok] is already sorted.
   Pass ok = 0 (or 1) if you don't know.
   It's sorted in-place, by a stable binary insertion sort. If ss->values
   isn't NULL, it's permuted in lockstep with ss->keys.
   On entry, must have n >= 1, and 0 <= ok <= n <= MAX_MINRUN.
   Return -1 if comparison raises an exception, else 0.
   Even in case of error, the output slice will be some permutation of
   the input (nothing is lost or duplicated): the pivot is only removed from
   a[ok] after its insertion point has been found, so a failing compare
   leaves a[ok] (and v[ok]) untouched.
*/
static int qp_ts_binarysort(qp_ts_MergeState *ms, const qp_ts_sortslice *ss,
                            Py_ssize_t n, Py_ssize_t ok)
{
    Py_ssize_t k; /* for IFLT macro expansion */
    JSValue * const a = ss->keys;
    JSValue * const v = ss->values;
    const BOOL has_values = v != NULL;
    JSValue pivot;
    Py_ssize_t M;
    Py_ssize_t L, R;

    assert(0 <= ok && ok <= n && 1 <= n && n <= MAX_MINRUN);
    /* assert a[:ok] is sorted */
    if (! ok)
        ++ok;
    /* Binary insertion sort (CPython's selected variant): worst, average and
       best case O(n log n) compares, O(n**2) data movement, which is cheap
       for n <= MAX_MINRUN. */
    for (; ok < n; ++ok) {
        /* set L to where a[ok] belongs */
        L = 0;
        R = ok;
        pivot = a[ok];
        /* Slice invariants. vacuously true at the start:
         * all a[0:L]  <= pivot
         * all a[L:R]     unknown
         * all a[R:ok]  > pivot
         */
        assert(L < R);
        do {
            /* L and R are very far from overflowing a Py_ssize_t */
            M = (L + R) >> 1;
            IFLT(pivot, a[M])
                R = M;
            else
                L = M + 1;
        } while (L < R);
        assert(L == R);
        /* a[:L] holds all elements from a[:ok] <= pivot now, so pivot belongs
           at index L. Slide a[L:ok] to the right a slot to make room for it. */
        for (M = ok; M > L; --M)
            a[M] = a[M - 1];
        a[L] = pivot;
        if (has_values) {
            pivot = v[ok];
            for (M = ok; M > L; --M)
                v[M] = v[M - 1];
            v[L] = pivot;
        }
    }
    return 0;

 fail:
    return -1;
}

/* Reverse the first n entries of a sortslice (keys and values together). */
static void qp_ts_sortslice_reverse(qp_ts_sortslice *s, Py_ssize_t n)
{
    qp_ts_reverse_slice(s->keys, &s->keys[n]);
    if (s->values != NULL)
        qp_ts_reverse_slice(s->values, &s->values[n]);
}

/*
Return the length of the run beginning at slo->keys, spanning no more than
nremaining elements. The run beginning there may be ascending or descending,
but the function permutes it in place, if needed, so that it's always ascending
upon return.  A descending run is strictly descending as far as stability
goes: equal neighbours inside it are kept in their original order (each
all-equal stretch is pre-reversed so that the final whole-run reversal
restores it).

Returns -1 in case of error (the slice is then some permutation of its input:
only whole reversals of sub-slices have been applied).
*/
static Py_ssize_t qp_ts_count_run(qp_ts_MergeState *ms, qp_ts_sortslice *slo,
                                  Py_ssize_t nremaining)
{
    Py_ssize_t k; /* used by IFLT macro expansion */
    Py_ssize_t n;
    JSValue * const lo = slo->keys;
    Py_ssize_t neq;

    /* In general, as things go on we've established that the slice starts
       with a monotone run of n elements, starting at lo. */

    /* We're n elements into the slice, and the most recent neq+1 elements are
     * all equal. This reverses them in-place, and resets neq for reuse.
     */
#define REVERSE_LAST_NEQ                                \
    if (neq) {                                          \
        qp_ts_sortslice slice = *slo;                   \
        ++neq;                                          \
        qp_ts_sortslice_advance(&slice, n - neq);       \
        qp_ts_sortslice_reverse(&slice, neq);           \
        neq = 0;                                        \
    }

    /* Sticking to only __lt__ compares is confusing and error-prone. But in
     * this routine, almost all uses of IFLT can be captured by tiny macros
     * giving mnemonic names to the intent. Note that inline functions don't
     * work for this (IFLT expands to code including `goto fail`).
     */
#define IF_NEXT_LARGER  IFLT(lo[n-1], lo[n])
#define IF_NEXT_SMALLER IFLT(lo[n], lo[n-1])

    assert(nremaining);
    /* try ascending run first */
    for (n = 1; n < nremaining; ++n) {
        IF_NEXT_SMALLER
            break;
    }
    if (n == nremaining)
        return n;
    /* lo[n] is strictly less */
    /* If n is 1 now, then the first compare established it's a descending
     * run, so fall through to the descending case. But if n > 1, there are
     * n elements in an ascending run terminated by the strictly less lo[n].
     * If the first key < lo[n-1], *somewhere* along the way the sequence
     * increased, so we're done (there is no descending run).
     * Else first key >= lo[n-1], which implies that the entire ascending run
     * consists of equal elements. In that case, this is a descending run,
     * and we reverse the all-equal prefix in-place.
     */
    if (n > 1) {
        IFLT(lo[0], lo[n-1])
            return n;
        qp_ts_sortslice_reverse(slo, n);
    }
    ++n; /* in all cases it's been established that lo[n] has been resolved */

    /* Finish descending run. All-equal subruns are reversed in-place on the
     * fly. Their original order will be restored at the end by the whole-slice
     * reversal.
     */
    neq = 0;
    for ( ; n < nremaining; ++n) {
        IF_NEXT_SMALLER {
            /* This ends the most recent run of equal elements, but still in
             * the "descending" direction.
             */
            REVERSE_LAST_NEQ
        }
        else {
            IF_NEXT_LARGER /* descending run is over */
                break;
            else /* not x < y and not y < x implies x == y */
                ++neq;
        }
    }
    REVERSE_LAST_NEQ
    qp_ts_sortslice_reverse(slo, n); /* transform to ascending run */

    /* And after reversing, it's possible this can be extended by a
     * naturally increasing suffix; e.g., [3, 2, 3, 4, 1] makes an
     * ascending run from the first 4 elements.
     */
    for ( ; n < nremaining; ++n) {
        IF_NEXT_SMALLER
            break;
    }

    return n;
fail:
    return -1;

#undef REVERSE_LAST_NEQ
#undef IF_NEXT_SMALLER
#undef IF_NEXT_LARGER
}

/*
Locate the proper position of key in a sorted vector; if the vector contains
an element equal to key, return the position immediately to the left of
the leftmost equal element.  [gallop_right() does the same except returns
the position to the right of the rightmost equal element (if any).]

"a" is a sorted vector with n elements, starting at a[0].  n must be > 0.

"hint" is an index at which to begin the search, 0 <= hint < n.  The closer
hint is to the final result, the faster this runs.

The return value is the int k in 0..n such that

    a[k-1] < key <= a[k]

pretending that *(a-1) is minus infinity and a[n] is plus infinity.  IOW,
key belongs at index k; or, IOW, the first k elements of a should precede
key, and the last n-k should follow key.

Returns -1 on error.  See listsort.txt for info on the method.
*/
static Py_ssize_t qp_ts_gallop_left(qp_ts_MergeState *ms, JSValue key,
                                    JSValue *a, Py_ssize_t n, Py_ssize_t hint)
{
    Py_ssize_t ofs;
    Py_ssize_t lastofs;
    Py_ssize_t k;

    assert(a && n > 0 && hint >= 0 && hint < n);

    a += hint;
    lastofs = 0;
    ofs = 1;
    IFLT(*a, key) {
        /* a[hint] < key -- gallop right, until
         * a[hint + lastofs] < key <= a[hint + ofs]
         */
        const Py_ssize_t maxofs = n - hint;             /* &a[n-1] is highest */
        while (ofs < maxofs) {
            IFLT(a[ofs], key) {
                lastofs = ofs;
                assert(ofs <= (PY_SSIZE_T_MAX - 1) / 2);
                ofs = (ofs << 1) + 1;
            }
            else                /* key <= a[hint + ofs] */
                break;
        }
        if (ofs > maxofs)
            ofs = maxofs;
        /* Translate back to offsets relative to &a[0]. */
        lastofs += hint;
        ofs += hint;
    }
    else {
        /* key <= a[hint] -- gallop left, until
         * a[hint - ofs] < key <= a[hint - lastofs]
         */
        const Py_ssize_t maxofs = hint + 1;             /* &a[0] is lowest */
        while (ofs < maxofs) {
            IFLT(*(a-ofs), key)
                break;
            /* key <= a[hint - ofs] */
            lastofs = ofs;
            assert(ofs <= (PY_SSIZE_T_MAX - 1) / 2);
            ofs = (ofs << 1) + 1;
        }
        if (ofs > maxofs)
            ofs = maxofs;
        /* Translate back to positive offsets relative to &a[0]. */
        k = lastofs;
        lastofs = hint - ofs;
        ofs = hint - k;
    }
    a -= hint;

    assert(-1 <= lastofs && lastofs < ofs && ofs <= n);
    /* Now a[lastofs] < key <= a[ofs], so key belongs somewhere to the
     * right of lastofs but no farther right than ofs.  Do a binary
     * search, with invariant a[lastofs-1] < key <= a[ofs].
     */
    ++lastofs;
    while (lastofs < ofs) {
        Py_ssize_t m = lastofs + ((ofs - lastofs) >> 1);

        IFLT(a[m], key)
            lastofs = m+1;              /* a[m] < key */
        else
            ofs = m;                    /* key <= a[m] */
    }
    assert(lastofs == ofs);             /* so a[ofs-1] < key <= a[ofs] */
    return ofs;

fail:
    return -1;
}

/*
Exactly like gallop_left(), except that if key already exists in a[0:n],
finds the position immediately to the right of the rightmost equal value.

The return value is the int k in 0..n such that

    a[k-1] <= key < a[k]

or -1 if error.

The code duplication is massive, but this is enough different given that
we're sticking to "<" comparisons that it's much harder to follow if
written as one routine with yet another "left or right?" flag.
*/
static Py_ssize_t qp_ts_gallop_right(qp_ts_MergeState *ms, JSValue key,
                                     JSValue *a, Py_ssize_t n, Py_ssize_t hint)
{
    Py_ssize_t ofs;
    Py_ssize_t lastofs;
    Py_ssize_t k;

    assert(a && n > 0 && hint >= 0 && hint < n);

    a += hint;
    lastofs = 0;
    ofs = 1;
    IFLT(key, *a) {
        /* key < a[hint] -- gallop left, until
         * a[hint - ofs] <= key < a[hint - lastofs]
         */
        const Py_ssize_t maxofs = hint + 1;             /* &a[0] is lowest */
        while (ofs < maxofs) {
            IFLT(key, *(a-ofs)) {
                lastofs = ofs;
                assert(ofs <= (PY_SSIZE_T_MAX - 1) / 2);
                ofs = (ofs << 1) + 1;
            }
            else                /* a[hint - ofs] <= key */
                break;
        }
        if (ofs > maxofs)
            ofs = maxofs;
        /* Translate back to positive offsets relative to &a[0]. */
        k = lastofs;
        lastofs = hint - ofs;
        ofs = hint - k;
    }
    else {
        /* a[hint] <= key -- gallop right, until
         * a[hint + lastofs] <= key < a[hint + ofs]
        */
        const Py_ssize_t maxofs = n - hint;             /* &a[n-1] is highest */
        while (ofs < maxofs) {
            IFLT(key, a[ofs])
                break;
            /* a[hint + ofs] <= key */
            lastofs = ofs;
            assert(ofs <= (PY_SSIZE_T_MAX - 1) / 2);
            ofs = (ofs << 1) + 1;
        }
        if (ofs > maxofs)
            ofs = maxofs;
        /* Translate back to offsets relative to &a[0]. */
        lastofs += hint;
        ofs += hint;
    }
    a -= hint;

    assert(-1 <= lastofs && lastofs < ofs && ofs <= n);
    /* Now a[lastofs] <= key < a[ofs], so key belongs somewhere to the
     * right of lastofs but no farther right than ofs.  Do a binary
     * search, with invariant a[lastofs-1] <= key < a[ofs].
     */
    ++lastofs;
    while (lastofs < ofs) {
        Py_ssize_t m = lastofs + ((ofs - lastofs) >> 1);

        IFLT(key, a[m])
            ofs = m;                    /* key < a[m] */
        else
            lastofs = m+1;              /* a[m] <= key */
    }
    assert(lastofs == ofs);             /* so a[ofs-1] <= key < a[ofs] */
    return ofs;

fail:
    return -1;
}

/* Conceptually a MergeState's constructor.  has_values says whether a values
 * array rides along with the keys (CPython: has_keyfunc); the temp area is
 * then split between keys and values. */
static void qp_ts_merge_init(qp_ts_MergeState *ms, JSContext *ctx,
                             qp_sort_lt_fn *lt, void *opaque,
                             Py_ssize_t list_size, int has_values,
                             qp_ts_sortslice *lo)
{
    assert(ms != NULL);
    if (has_values) {
        /* The temporary space for merging will need at most half the list
         * size rounded up.
         */
        ms->alloced = (list_size + 1) / 2;

        /* ms->alloced describes how many keys will be stored at
           ms->temparray, but we also need to store the values.  Hence,
           ms->alloced is capped at half of MERGESTATE_TEMP_SIZE. */
        if (MERGESTATE_TEMP_SIZE / 2 < ms->alloced)
            ms->alloced = MERGESTATE_TEMP_SIZE / 2;
        ms->a.values = &ms->temparray[ms->alloced];
    }
    else {
        ms->alloced = MERGESTATE_TEMP_SIZE;
        ms->a.values = NULL;
    }
    ms->a.keys = ms->temparray;
    ms->n = 0;
    ms->min_gallop = MIN_GALLOP;
    ms->listlen = list_size;
    ms->basekeys = lo->keys;
    ms->ctx = ctx;
    ms->lt = lt;
    ms->opaque = opaque;

    /* State for generating minrun values. See listsort.txt. */
    ms->mr_e = 0;
    while (list_size >> ms->mr_e >= MAX_MINRUN) {
        ++ms->mr_e;
    }
    ms->mr_mask = ((Py_ssize_t)1 << ms->mr_e) - 1;
    ms->mr_current = 0;
}

/* Free all the temp memory owned by the MergeState.  This must be called
 * when you're done with a MergeState, and may be called before then if
 * you want to free the temp memory early.
 */
static void qp_ts_merge_freemem(qp_ts_MergeState *ms)
{
    assert(ms != NULL);
    if (ms->a.keys != ms->temparray) {
        js_free(ms->ctx, ms->a.keys);
        ms->a.keys = NULL;
    }
}

/* Ensure enough temp memory for 'need' array slots is available.
 * Returns 0 on success and -1 (out-of-memory exception pending on ctx) if the
 * memory can't be gotten.  The old contents are not preserved.
 */
static int qp_ts_merge_getmem(qp_ts_MergeState *ms, Py_ssize_t need)
{
    int multiplier;

    assert(ms != NULL);
    if (need <= ms->alloced)
        return 0;

    multiplier = ms->a.values != NULL ? 2 : 1;

    /* Don't realloc!  That can cost cycles to copy the old data, but
     * we don't care what's in the block.
     */
    qp_ts_merge_freemem(ms);
    if ((size_t)need > PY_SSIZE_T_MAX / sizeof(JSValue) / multiplier) {
        /* ms->a.keys is NULL here; merge_freemem copes with that. */
        JS_ThrowOutOfMemory(ms->ctx);
        return -1;
    }
    ms->a.keys = js_malloc(ms->ctx, (size_t)multiplier * need * sizeof(JSValue));
    if (ms->a.keys != NULL) {
        ms->alloced = need;
        if (ms->a.values != NULL)
            ms->a.values = &ms->a.keys[need];
        return 0;
    }
    /* js_malloc has thrown the out-of-memory exception. */
    return -1;
}
#define MERGE_GETMEM(MS, NEED) ((NEED) <= (MS)->alloced ? 0 :   \
                                qp_ts_merge_getmem(MS, NEED))

/* Merge the na elements starting at ssa with the nb elements starting at
 * ssb.keys = ssa.keys + na in a stable way, in-place.  na and nb must be > 0.
 * Must also have that ssa.keys[na-1] belongs at the end of the merge, and
 * should have na <= nb.  See listsort.txt for more info.  Return 0 if
 * successful, -1 if error.  On error the not-yet-merged part of run A is
 * copied back from the temp area, so the slice stays a permutation.
 */
static Py_ssize_t qp_ts_merge_lo(qp_ts_MergeState *ms, qp_ts_sortslice ssa,
                                 Py_ssize_t na, qp_ts_sortslice ssb,
                                 Py_ssize_t nb)
{
    Py_ssize_t k;
    qp_ts_sortslice dest;
    int result = -1;            /* guilty until proved innocent */
    Py_ssize_t min_gallop;

    assert(ms && ssa.keys && ssb.keys && na > 0 && nb > 0);
    assert(ssa.keys + na == ssb.keys);
    if (MERGE_GETMEM(ms, na) < 0)
        return -1;
    qp_ts_sortslice_memcpy(&ms->a, 0, &ssa, 0, na);
    dest = ssa;
    ssa = ms->a;

    qp_ts_sortslice_copy_incr(&dest, &ssb);
    --nb;
    if (nb == 0)
        goto Succeed;
    if (na == 1)
        goto CopyB;

    min_gallop = ms->min_gallop;
    for (;;) {
        Py_ssize_t acount = 0;          /* # of times A won in a row */
        Py_ssize_t bcount = 0;          /* # of times B won in a row */

        /* Do the straightforward thing until (if ever) one run
         * appears to win consistently.
         */
        for (;;) {
            assert(na > 1 && nb > 0);
            k = ISLT(ssb.keys[0], ssa.keys[0]);
            if (k) {
                if (k < 0)
                    goto Fail;
                qp_ts_sortslice_copy_incr(&dest, &ssb);
                ++bcount;
                acount = 0;
                --nb;
                if (nb == 0)
                    goto Succeed;
                if (bcount >= min_gallop)
                    break;
            }
            else {
                qp_ts_sortslice_copy_incr(&dest, &ssa);
                ++acount;
                bcount = 0;
                --na;
                if (na == 1)
                    goto CopyB;
                if (acount >= min_gallop)
                    break;
            }
        }

        /* One run is winning so consistently that galloping may
         * be a huge win.  So try that, and continue galloping until
         * (if ever) neither run appears to be winning consistently
         * anymore.
         */
        ++min_gallop;
        do {
            assert(na > 1 && nb > 0);
            min_gallop -= min_gallop > 1;
            ms->min_gallop = min_gallop;
            k = qp_ts_gallop_right(ms, ssb.keys[0], ssa.keys, na, 0);
            acount = k;
            if (k) {
                if (k < 0)
                    goto Fail;
                qp_ts_sortslice_memcpy(&dest, 0, &ssa, 0, k);
                qp_ts_sortslice_advance(&dest, k);
                qp_ts_sortslice_advance(&ssa, k);
                na -= k;
                if (na == 1)
                    goto CopyB;
                /* na==0 is impossible now if the comparison
                 * function is consistent, but we can't assume
                 * that it is.
                 */
                if (na == 0)
                    goto Succeed;
            }
            qp_ts_sortslice_copy_incr(&dest, &ssb);
            --nb;
            if (nb == 0)
                goto Succeed;

            k = qp_ts_gallop_left(ms, ssa.keys[0], ssb.keys, nb, 0);
            bcount = k;
            if (k) {
                if (k < 0)
                    goto Fail;
                qp_ts_sortslice_memmove(&dest, 0, &ssb, 0, k);
                qp_ts_sortslice_advance(&dest, k);
                qp_ts_sortslice_advance(&ssb, k);
                nb -= k;
                if (nb == 0)
                    goto Succeed;
            }
            qp_ts_sortslice_copy_incr(&dest, &ssa);
            --na;
            if (na == 1)
                goto CopyB;
        } while (acount >= MIN_GALLOP || bcount >= MIN_GALLOP);
        ++min_gallop;           /* penalize it for leaving galloping mode */
        ms->min_gallop = min_gallop;
    }
Succeed:
    result = 0;
Fail:
    if (na)
        qp_ts_sortslice_memcpy(&dest, 0, &ssa, 0, na);
    return result;
CopyB:
    assert(na == 1 && nb > 0);
    /* The last element of ssa belongs at the end of the merge. */
    qp_ts_sortslice_memmove(&dest, 0, &ssb, 0, nb);
    qp_ts_sortslice_copy(&dest, nb, &ssa, 0);
    return 0;
}

/* Merge the na elements starting at pa with the nb elements starting at
 * ssb.keys = ssa.keys + na in a stable way, in-place.  na and nb must be > 0.
 * Must also have that ssa.keys[na-1] belongs at the end of the merge, and
 * should have na >= nb.  See listsort.txt for more info.  Return 0 if
 * successful, -1 if error.  On error the not-yet-merged part of run B is
 * copied back from the temp area, so the slice stays a permutation.
 */
static Py_ssize_t qp_ts_merge_hi(qp_ts_MergeState *ms, qp_ts_sortslice ssa,
                                 Py_ssize_t na, qp_ts_sortslice ssb,
                                 Py_ssize_t nb)
{
    Py_ssize_t k;
    qp_ts_sortslice dest, basea, baseb;
    int result = -1;            /* guilty until proved innocent */
    Py_ssize_t min_gallop;

    assert(ms && ssa.keys && ssb.keys && na > 0 && nb > 0);
    assert(ssa.keys + na == ssb.keys);
    if (MERGE_GETMEM(ms, nb) < 0)
        return -1;
    dest = ssb;
    qp_ts_sortslice_advance(&dest, nb-1);
    qp_ts_sortslice_memcpy(&ms->a, 0, &ssb, 0, nb);
    basea = ssa;
    baseb = ms->a;
    ssb.keys = ms->a.keys + nb - 1;
    if (ssb.values != NULL)
        ssb.values = ms->a.values + nb - 1;
    qp_ts_sortslice_advance(&ssa, na - 1);

    qp_ts_sortslice_copy_decr(&dest, &ssa);
    --na;
    if (na == 0)
        goto Succeed;
    if (nb == 1)
        goto CopyA;

    min_gallop = ms->min_gallop;
    for (;;) {
        Py_ssize_t acount = 0;          /* # of times A won in a row */
        Py_ssize_t bcount = 0;          /* # of times B won in a row */

        /* Do the straightforward thing until (if ever) one run
         * appears to win consistently.
         */
        for (;;) {
            assert(na > 0 && nb > 1);
            k = ISLT(ssb.keys[0], ssa.keys[0]);
            if (k) {
                if (k < 0)
                    goto Fail;
                qp_ts_sortslice_copy_decr(&dest, &ssa);
                ++acount;
                bcount = 0;
                --na;
                if (na == 0)
                    goto Succeed;
                if (acount >= min_gallop)
                    break;
            }
            else {
                qp_ts_sortslice_copy_decr(&dest, &ssb);
                ++bcount;
                acount = 0;
                --nb;
                if (nb == 1)
                    goto CopyA;
                if (bcount >= min_gallop)
                    break;
            }
        }

        /* One run is winning so consistently that galloping may
         * be a huge win.  So try that, and continue galloping until
         * (if ever) neither run appears to be winning consistently
         * anymore.
         */
        ++min_gallop;
        do {
            assert(na > 0 && nb > 1);
            min_gallop -= min_gallop > 1;
            ms->min_gallop = min_gallop;
            k = qp_ts_gallop_right(ms, ssb.keys[0], basea.keys, na, na-1);
            if (k < 0)
                goto Fail;
            k = na - k;
            acount = k;
            if (k) {
                qp_ts_sortslice_advance(&dest, -k);
                qp_ts_sortslice_advance(&ssa, -k);
                qp_ts_sortslice_memmove(&dest, 1, &ssa, 1, k);
                na -= k;
                if (na == 0)
                    goto Succeed;
            }
            qp_ts_sortslice_copy_decr(&dest, &ssb);
            --nb;
            if (nb == 1)
                goto CopyA;

            k = qp_ts_gallop_left(ms, ssa.keys[0], baseb.keys, nb, nb-1);
            if (k < 0)
                goto Fail;
            k = nb - k;
            bcount = k;
            if (k) {
                qp_ts_sortslice_advance(&dest, -k);
                qp_ts_sortslice_advance(&ssb, -k);
                qp_ts_sortslice_memcpy(&dest, 1, &ssb, 1, k);
                nb -= k;
                if (nb == 1)
                    goto CopyA;
                /* nb==0 is impossible now if the comparison
                 * function is consistent, but we can't assume
                 * that it is.
                 */
                if (nb == 0)
                    goto Succeed;
            }
            qp_ts_sortslice_copy_decr(&dest, &ssa);
            --na;
            if (na == 0)
                goto Succeed;
        } while (acount >= MIN_GALLOP || bcount >= MIN_GALLOP);
        ++min_gallop;           /* penalize it for leaving galloping mode */
        ms->min_gallop = min_gallop;
    }
Succeed:
    result = 0;
Fail:
    if (nb)
        qp_ts_sortslice_memcpy(&dest, -(nb-1), &baseb, 0, nb);
    return result;
CopyA:
    assert(nb == 1 && na > 0);
    /* The first element of ssb belongs at the front of the merge. */
    qp_ts_sortslice_memmove(&dest, 1-na, &ssa, 1-na, na);
    qp_ts_sortslice_advance(&dest, -na);
    qp_ts_sortslice_advance(&ssa, -na);
    qp_ts_sortslice_copy(&dest, 0, &ssb, 0);
    return 0;
}

/* Merge the two runs at stack indices i and i+1.
 * Returns 0 on success, -1 on error.
 */
static Py_ssize_t qp_ts_merge_at(qp_ts_MergeState *ms, Py_ssize_t i)
{
    qp_ts_sortslice ssa, ssb;
    Py_ssize_t na, nb;
    Py_ssize_t k;

    assert(ms != NULL);
    assert(ms->n >= 2);
    assert(i >= 0);
    assert(i == ms->n - 2 || i == ms->n - 3);

    ssa = ms->pending[i].base;
    na = ms->pending[i].len;
    ssb = ms->pending[i+1].base;
    nb = ms->pending[i+1].len;
    assert(na > 0 && nb > 0);
    assert(ssa.keys + na == ssb.keys);

    /* Record the length of the combined runs; if i is the 3rd-last
     * run now, also slide over the last run (which isn't involved
     * in this merge).  The current run i+1 goes away in any case.
     */
    ms->pending[i].len = na + nb;
    if (i == ms->n - 3)
        ms->pending[i+1] = ms->pending[i+2];
    --ms->n;

    /* Where does b start in a?  Elements in a before that can be
     * ignored (already in place).
     */
    k = qp_ts_gallop_right(ms, *ssb.keys, ssa.keys, na, 0);
    if (k < 0)
        return -1;
    qp_ts_sortslice_advance(&ssa, k);
    na -= k;
    if (na == 0)
        return 0;

    /* Where does a end in b?  Elements in b after that can be
     * ignored (already in place).
     */
    nb = qp_ts_gallop_left(ms, ssa.keys[na-1], ssb.keys, nb, nb-1);
    if (nb <= 0)
        return nb;

    /* Merge what remains of the runs, using a temp array with
     * min(na, nb) elements.
     */
    if (na <= nb)
        return qp_ts_merge_lo(ms, ssa, na, ssb, nb);
    else
        return qp_ts_merge_hi(ms, ssa, na, ssb, nb);
}

/* Two adjacent runs begin at index s1. The first run has length n1, and
 * the second run (starting at index s1+n1) has length n2. The list has total
 * length n.
 * Compute the "power" of the first run. See listsort.txt for details.
 */
static int qp_ts_powerloop(Py_ssize_t s1, Py_ssize_t n1, Py_ssize_t n2,
                           Py_ssize_t n)
{
    int result = 0;
    Py_ssize_t a, b;
    assert(s1 >= 0);
    assert(n1 > 0 && n2 > 0);
    assert(s1 + n1 + n2 <= n);
    /* midpoints a and b:
     * a = s1 + n1/2
     * b = s1 + n1 + n2/2 = a + (n1 + n2)/2
     *
     * Those may not be integers, though, because of the "/2". So we work with
     * 2*a and 2*b instead, which are necessarily integers. It makes no
     * difference to the outcome, since the bits in the expansion of (2*i)/n
     * are merely shifted one position from those of i/n.
     */
    a = 2 * s1 + n1;  /* 2*a */
    b = a + n1 + n2;  /* 2*b */
    /* Emulate a/n and b/n one bit a time, until bits differ. */
    for (;;) {
        ++result;
        if (a >= n) {  /* both quotient bits are 1 */
            assert(b >= a);
            a -= n;
            b -= n;
        }
        else if (b >= n) {  /* a/n bit is 0, b/n bit is 1 */
            break;
        } /* else both quotient bits are 0 */
        assert(a < b && b < n);
        a <<= 1;
        b <<= 1;
    }
    return result;
}

/* The next run has been identified, of length n2.
 * If there's already a run on the stack, apply the "powersort" merge strategy:
 * compute the topmost run's "power" (depth in a conceptual binary merge tree)
 * and merge adjacent runs on the stack with greater power. See listsort.txt
 * for more info.
 *
 * It's the caller's responsibility to push the new run on the stack when this
 * returns.
 *
 * Returns 0 on success, -1 on error.
 */
static int qp_ts_found_new_run(qp_ts_MergeState *ms, Py_ssize_t n2)
{
    assert(ms);
    if (ms->n) {
        struct qp_ts_slice *p = ms->pending;
        Py_ssize_t s1 = p[ms->n - 1].base.keys - ms->basekeys; /* start index */
        Py_ssize_t n1 = p[ms->n - 1].len;
        int power = qp_ts_powerloop(s1, n1, n2, ms->listlen);
        assert(ms->n > 0);
        while (ms->n > 1 && p[ms->n - 2].power > power) {
            if (qp_ts_merge_at(ms, ms->n - 2) < 0)
                return -1;
        }
        assert(ms->n < 2 || p[ms->n - 2].power < power);
        p[ms->n - 1].power = power;
    }
    return 0;
}

/* Regardless of invariants, merge all runs on the stack until only one
 * remains.  This is used at the end of the mergesort.
 *
 * Returns 0 on success, -1 on error.
 */
static int qp_ts_merge_force_collapse(qp_ts_MergeState *ms)
{
    struct qp_ts_slice *p = ms->pending;

    assert(ms);
    while (ms->n > 1) {
        Py_ssize_t n = ms->n - 2;
        if (n > 0 && p[n-1].len < p[n+1].len)
            --n;
        if (qp_ts_merge_at(ms, n) < 0)
            return -1;
    }
    return 0;
}

/* Return the next minrun value to use. See listsort.txt. */
static inline Py_ssize_t qp_ts_minrun_next(qp_ts_MergeState *ms)
{
    Py_ssize_t result;
    ms->mr_current += ms->listlen;
    assert(ms->mr_current >= 0); /* no overflow */
    result = ms->mr_current >> ms->mr_e;
    ms->mr_current &= ms->mr_mask;
    return result;
}

/* An adaptive, stable, natural mergesort.  See listsort.txt and sort.h.
 * This is the core of CPython's list_sort_impl, from merge_init to
 * merge_force_collapse.  Returns 0 on success, -1 on error.  Even in case of
 * error, the arrays will be some permutation of their input state (nothing is
 * lost or duplicated), with keys[i] still paired with values[i].
 */
static int qp_timsort(JSContext *ctx, JSValue *keys, JSValue *values,
                      Py_ssize_t n, qp_sort_lt_fn *lt, void *opaque)
{
    qp_ts_MergeState ms;
    Py_ssize_t nremaining;
    Py_ssize_t minrun;
    qp_ts_sortslice lo;
    int result = -1;

    lo.keys = keys;
    lo.values = values;
    qp_ts_merge_init(&ms, ctx, lt, opaque, n, values != NULL, &lo);

    nremaining = n;
    if (nremaining < 2)
        goto succeed;

    /* March over the array once, left to right, finding natural runs,
     * and extending short natural runs to minrun elements.
     */
    do {
        Py_ssize_t r;

        /* Identify next run. */
        r = qp_ts_count_run(&ms, &lo, nremaining);
        if (r < 0)
            goto fail;
        /* If short, extend to min(minrun, nremaining). */
        minrun = qp_ts_minrun_next(&ms);
        if (r < minrun) {
            const Py_ssize_t force = nremaining <= minrun ?
                              nremaining : minrun;
            if (qp_ts_binarysort(&ms, &lo, force, r) < 0)
                goto fail;
            r = force;
        }
        /* Maybe merge pending runs. */
        assert(ms.n == 0 || ms.pending[ms.n -1].base.keys +
                            ms.pending[ms.n-1].len == lo.keys);
        if (qp_ts_found_new_run(&ms, r) < 0)
            goto fail;
        /* Push new run on stack. */
        assert((size_t)ms.n < MAX_MERGE_PENDING);
        ms.pending[ms.n].base = lo;
        ms.pending[ms.n].len = r;
        ++ms.n;
        /* Advance to find next run. */
        qp_ts_sortslice_advance(&lo, r);
        nremaining -= r;
    } while (nremaining);

    if (qp_ts_merge_force_collapse(&ms) < 0)
        goto fail;
    assert(ms.n == 1);
    assert(ms.pending[0].base.keys == keys);
    assert(ms.pending[0].len == n);

succeed:
    result = 0;
fail:
    qp_ts_merge_freemem(&ms);
    return result;
}

/* ---- Pre-sort homogeneity check and the comparisons it selects ----
 *
 * CPython (list_sort_impl, "The pre-sort check") proves once per sort that
 * all keys have one exact type and then uses unsafe_long_compare,
 * unsafe_float_compare or unsafe_latin_compare instead of
 * PyObject_RichCompareBool.  They compute the same answer without
 * dispatching, so the algorithm makes the same compares in the same order.
 * The equivalents below work on JSValue tags (see sort.h for the type
 * mapping); none of them can fail.
 */

/* Value of a JS_TAG_INT or JS_TAG_SHORT_BIG_INT (both exact Python int). */
static inline int64_t qp_sort_small_int(JSValueConst v)
{
    if (JS_VALUE_GET_TAG(v) == JS_TAG_INT)
        return JS_VALUE_GET_INT(v);
    return (int64_t)JS_VALUE_GET_SHORT_BIG_INT(v);
}

/* Bounded int compare (CPython unsafe_long_compare). */
static int qp_sort_int_lt(JSContext *ctx, JSValueConst a, JSValueConst b,
                          void *opaque)
{
    return qp_sort_small_int(a) < qp_sort_small_int(b);
}

/* Float compare (CPython unsafe_float_compare): C `<` on doubles is exactly
 * float.__lt__, including NaN (always false) and -0.0 == 0.0. */
static int qp_sort_float_lt(JSContext *ctx, JSValueConst a, JSValueConst b,
                            void *opaque)
{
    return JS_VALUE_GET_FLOAT64(a) < JS_VALUE_GET_FLOAT64(b);
}

/* Latin string compare (CPython unsafe_latin_compare): 8-bit JSString bytes
 * are latin-1 code points, so memcmp order is code-point order; on a common
 * prefix the shorter string is smaller. */
static int qp_sort_latin1_lt(JSContext *ctx, JSValueConst a, JSValueConst b,
                             void *opaque)
{
    const JSString *p1 = JS_VALUE_GET_STRING(a);
    const JSString *p2 = JS_VALUE_GET_STRING(b);
    uint32_t len = min_uint32(p1->len, p2->len);
    int res = memcmp(p1->u.str8, p2->u.str8, len);

    return res != 0 ? res < 0 : p1->len < p2->len;
}

/* Fold one key into the homogeneity state (see sort.h).  The first key
 * decides the candidate kind; any key of another kind makes the result
 * MIXED, which is absorbing. */
static qp_sort_kind qp_sort_kind_add(qp_sort_kind kind, JSValueConst key)
{
    qp_sort_kind k;

    if (kind == QP_SORT_KIND_MIXED)
        return kind;
    switch (JS_VALUE_GET_NORM_TAG(key)) {
    case JS_TAG_INT:
    case JS_TAG_SHORT_BIG_INT:
        k = QP_SORT_KIND_INT;
        break;
    case JS_TAG_FLOAT64:
        k = QP_SORT_KIND_FLOAT;
        break;
    case JS_TAG_STRING:
        k = JS_VALUE_GET_STRING(key)->is_wide_char ? QP_SORT_KIND_MIXED
                                                   : QP_SORT_KIND_LATIN1;
        break;
    default:
        k = QP_SORT_KIND_MIXED;
        break;
    }
    if (kind == QP_SORT_KIND_EMPTY || kind == k)
        return k;
    return QP_SORT_KIND_MIXED;
}

/* The never-failing comparison valid for every key folded into `kind`, or
 * NULL when the generic Python `<` must be used. */
static qp_sort_lt_fn *qp_sort_kind_lt(qp_sort_kind kind)
{
    switch (kind) {
    case QP_SORT_KIND_INT:
        return qp_sort_int_lt;
    case QP_SORT_KIND_FLOAT:
        return qp_sort_float_lt;
    case QP_SORT_KIND_LATIN1:
        return qp_sort_latin1_lt;
    default:
        return NULL;
    }
}

static qp_sort_lt_fn *qp_sort_presort_check(const JSValue *keys, Py_ssize_t n)
{
    qp_sort_kind kind = QP_SORT_KIND_EMPTY;
    Py_ssize_t i;

    for (i = 0; i < n && kind != QP_SORT_KIND_MIXED; i++)
        kind = qp_sort_kind_add(kind, keys[i]);
    return qp_sort_kind_lt(kind);
}

#undef ISLT
#undef IFLT
#undef MAX_MERGE_PENDING
#undef MIN_GALLOP
#undef MERGESTATE_TEMP_SIZE
#undef MAX_MINRUN
#undef MERGE_GETMEM
