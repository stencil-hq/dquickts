/*
 * sort.h - CPython's list.sort (timsort / powersort) over JSValue arrays.
 *
 * Derived from CPython Objects/listobject.c (PSF-2.0).
 *
 * This file is part of the single quickjs.c translation unit (included through
 * python/quickpy.c), so everything here is `static`.  It only needs
 * <quickjs.h> internals (JSValue, JSContext, js_malloc) and Py_ssize_t from
 * python/compat.h.  It does not depend on the Python object model: callers
 * supply the strict "less than" comparison as a callback.
 *
 * Caller responsibilities (what CPython's list_sort_impl does around the
 * sort proper, and which qp_timsort deliberately does NOT do):
 *
 *  - key=: compute the keys array first (one key per item, in item order,
 *    stopping and cleaning up at the first failing key call), then pass it as
 *    `keys` with the items as `values`.  Without key=, pass the items as
 *    `keys` and `values == NULL`.  Free the keys afterwards.
 *  - reverse=True: CPython reverses the arrays (keys and values) BEFORE the
 *    sort and reverses the items again AFTER it, whether or not the sort
 *    failed (`if (reverse && n > 1)`).  That is what keeps reverse sorts
 *    stable; do exactly the same around qp_timsort (qp_sort_reverse helps).
 *  - Mutation: CPython detaches the list's storage for the duration of the
 *    sort (the list looks empty to `lt` callbacks) and raises
 *    ValueError("list modified during sort") if the list was touched.  The
 *    arrays passed here must likewise be private to the caller while sorting:
 *    `lt` may run arbitrary code, and it must not be able to reach (resize,
 *    free, or GC-mark) them.  In particular the arrays must not be scanned by
 *    the cycle collector's mark functions during the sort: while a merge is in
 *    progress some values live only in the internal temp buffer and others
 *    appear twice in the array.  The references held by the arrays are owned
 *    by the caller (they act as GC roots); qp_timsort only moves JSValues
 *    around and never duplicates or frees a reference.
 *  - The pre-sort homogeneity check (CPython picks unsafe_long_compare,
 *    unsafe_float_compare, unsafe_latin_compare, ... once per sort): see
 *    qp_sort_kind_* below.  It only replaces the comparison function by an
 *    equivalent one that cannot run user code, so it never changes the
 *    comparison sequence or the result.
 */
#ifndef QP_SORT_H
#define QP_SORT_H

/* Strict "a < b" on two keys: return 1 if a < b, 0 if not, -1 with an
 * exception pending on ctx.  `opaque` is passed through unchanged. */
typedef int qp_sort_lt_fn(JSContext *ctx, JSValueConst a, JSValueConst b,
                          void *opaque);

/* Sort keys[0:n] in place, stably, with CPython's listsort algorithm,
 * permuting values[0:n] (if non-NULL) in lockstep.  Compares are made only on
 * keys, with `lt`, in exactly the order CPython makes them.
 * Returns 0 on success, -1 on error (exception from `lt`, or out of memory via
 * js_malloc).  On error both arrays are still a permutation of their input
 * (nothing lost or duplicated) and keys[i] still corresponds to values[i]. */
static int qp_timsort(JSContext *ctx, JSValue *keys, JSValue *values,
                      Py_ssize_t n, qp_sort_lt_fn *lt, void *opaque);

/* Reverse a[0:n] in place (CPython reverse_slice); for reverse=True. */
static void qp_sort_reverse(JSValue *a, Py_ssize_t n);

/* Pre-sort type-homogeneity check.
 *
 * Fold every key with qp_sort_kind_add, starting from QP_SORT_KIND_EMPTY
 * (stop early once QP_SORT_KIND_MIXED is reached), then
 * qp_sort_kind_lt() returns a comparison that is valid for every pair of the
 * folded keys and never fails nor runs user code, or NULL if none applies
 * (EMPTY or MIXED) -- in which case use the generic Python `<`.
 * A tuple-aware caller can fold the first elements of non-empty tuple keys
 * instead and use the result for element 0 the way CPython's
 * unsafe_tuple_compare uses ms->tuple_elem_compare.
 *
 * Kinds are decided on the exact JSValue tag, which is the exact Python type
 * (plan D3): JS_TAG_INT / JS_TAG_SHORT_BIG_INT are `int` (CPython's "compact
 * ints" case; heap BigInts make the set MIXED), JS_TAG_FLOAT64 is `float`,
 * JS_TAG_STRING with 8-bit storage is a latin-1 `str` (ropes and 16-bit
 * strings make the set MIXED, like non-1-byte kinds in CPython).
 * Subclass instances are objects, never these tags. */
typedef enum qp_sort_kind {
    QP_SORT_KIND_EMPTY,     /* nothing folded yet */
    QP_SORT_KIND_INT,       /* all JS_TAG_INT / JS_TAG_SHORT_BIG_INT */
    QP_SORT_KIND_FLOAT,     /* all JS_TAG_FLOAT64 */
    QP_SORT_KIND_LATIN1,    /* all flat 8-bit JSStrings */
    QP_SORT_KIND_MIXED,     /* anything else: no fast comparison */
} qp_sort_kind;

static qp_sort_kind qp_sort_kind_add(qp_sort_kind kind, JSValueConst key);
static qp_sort_lt_fn *qp_sort_kind_lt(qp_sort_kind kind);
/* Convenience: fold keys[0:n] and return qp_sort_kind_lt of the result. */
static qp_sort_lt_fn *qp_sort_presort_check(const JSValue *keys, Py_ssize_t n);

#endif /* QP_SORT_H */
