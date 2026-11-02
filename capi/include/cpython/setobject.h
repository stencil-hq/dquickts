#ifndef Py_CPYTHON_SETOBJECT_H
#  error "this header file must not be included directly"
#endif

/* There are three kinds of entries in the table:

1. Unused:  key == NULL and hash == 0
2. Dummy:   key == dummy and hash == -1
3. Active:  key != NULL and key != dummy and hash != -1

The hash field of Unused slots is always zero.

The hash field of Dummy slots are set to -1
meaning that dummy entries can be detected by
either entry->key==dummy or by entry->hash==-1.
*/

#define PySet_MINSIZE 8

typedef struct {
    PyObject *key;
    Py_hash_t hash;             /* Cached hash code of the key */
} setentry;

/* The SetObject data structure is shared by set and frozenset objects.

Invariant for sets:
 - hash is -1

Invariants for frozensets:
 - data is immutable.
 - hash is the hash of the frozenset or -1 if not computed yet.

*/

/* dquickts (D5): sets are the shared JS PY_SET/PY_FROZENSET objects;
   PySetObject is the C-side handle.  The hash table is not exposed (fields
   are private runtime state: borrowed-key pins of _PySet_NextEntry*), and
   the size is a call. */
typedef struct {
    PyObject_HEAD
    void *_qp_pins;
    Py_ssize_t _qp_reserved;
} PySetObject;

#define _PySet_CAST(so) \
    (assert(PyAnySet_Check(so)), _Py_CAST(PySetObject*, so))

#define PySet_GET_SIZE(so) PySet_Size(_PyObject_CAST(so))
