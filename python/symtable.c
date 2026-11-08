/*
 * Python symbol table construction and analysis, __future__ detection and
 * private name mangling.
 *
 * Derived from CPython Python/symtable.c, Python/future.c and
 * Include/internal/pycore_symtable.h (PSF-2.0), v3.15.2.
 * See symtable.h for the differences from CPython (C containers instead of
 * Python objects, JSAtom identifiers, qp_error reporting).
 */

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

// Set this to 1 to dump all symtables to stdout for debugging
#define QP_DUMP_SYMTABLE 0

/* ======================================================================
   Containers
   ====================================================================== */

#define QP_SYMMAP_EMPTY (-1)
#define QP_SYMMAP_DUMMY (-2)

static inline uint32_t
qp_symmap_slot(JSAtom key, uint32_t mask)
{
    uint32_t h = (uint32_t)key * 0x9E3779B1u;
    return (h ^ (h >> 16)) & mask;
}

static void
qp_symmap_init(qp_symmap *m, JSRuntime *rt)
{
    memset(m, 0, sizeof(*m));
    m->rt = rt;
}

static void
qp_symmap_free(qp_symmap *m)
{
    if (m->entries)
        js_free_rt(m->rt, m->entries);
    if (m->index)
        js_free_rt(m->rt, m->index);
    m->entries = NULL;
    m->index = NULL;
    m->index_size = 0;
    m->n_entries = 0;
    m->size = 0;
}

static Py_ssize_t
qp_symmap_size(const qp_symmap *m)
{
    return m->size;
}

/* Index into m->entries of `key`, or -1. */
static int32_t
qp_symmap_find(const qp_symmap *m, JSAtom key)
{
    uint32_t mask, i;
    if (m->index_size == 0)
        return -1;
    mask = m->index_size - 1;
    i = qp_symmap_slot(key, mask);
    for (;;) {
        int32_t ix = m->index[i];
        if (ix == QP_SYMMAP_EMPTY)
            return -1;
        if (ix >= 0 && m->entries[ix].key == key)
            return ix;
        i = (i + 1) & mask;
    }
}

/* Rebuild the table with room for at least `min_live` keys, compacting
   deleted entries (insertion order is kept). */
static int
qp_symmap_rebuild(qp_symmap *m, uint32_t min_live)
{
    uint32_t new_size = 8, cap, mask, i, j;
    int32_t *index;
    qp_symmap_entry *entries;

    while ((uint64_t)min_live * 3 > (uint64_t)new_size * 2)
        new_size <<= 1;
    cap = new_size * 2 / 3;
    index = js_malloc_rt(m->rt, sizeof(int32_t) * new_size);
    if (!index)
        return -1;
    entries = js_malloc_rt(m->rt, sizeof(qp_symmap_entry) * cap);
    if (!entries) {
        js_free_rt(m->rt, index);
        return -1;
    }
    for (i = 0; i < new_size; i++)
        index[i] = QP_SYMMAP_EMPTY;
    mask = new_size - 1;
    for (i = 0, j = 0; i < m->n_entries; i++) {
        uint32_t k;
        if (m->entries[i].key == JS_ATOM_NULL)
            continue;
        entries[j] = m->entries[i];
        k = qp_symmap_slot(entries[j].key, mask);
        while (index[k] != QP_SYMMAP_EMPTY)
            k = (k + 1) & mask;
        index[k] = (int32_t)j;
        j++;
    }
    if (m->entries)
        js_free_rt(m->rt, m->entries);
    if (m->index)
        js_free_rt(m->rt, m->index);
    m->entries = entries;
    m->index = index;
    m->index_size = new_size;
    m->n_entries = j;
    m->size = j;
    return 0;
}

static int
qp_symmap_lookup(const qp_symmap *m, JSAtom key, long *pval)
{
    int32_t ix = qp_symmap_find(m, key);
    if (ix < 0)
        return 0;
    if (pval)
        *pval = m->entries[ix].value;
    return 1;
}

static int
qp_symmap_contains(const qp_symmap *m, JSAtom key)
{
    return qp_symmap_find(m, key) >= 0;
}

static int
qp_symmap_set(qp_symmap *m, JSAtom key, long value)
{
    uint32_t mask, i;
    int32_t ix = qp_symmap_find(m, key);
    if (ix >= 0) {
        m->entries[ix].value = value;
        return 0;
    }
    if (m->n_entries >= m->index_size * 2 / 3) {
        if (qp_symmap_rebuild(m, (m->size + 1) * 2) < 0)
            return -1;
    }
    mask = m->index_size - 1;
    i = qp_symmap_slot(key, mask);
    while (m->index[i] >= 0)
        i = (i + 1) & mask;
    m->index[i] = (int32_t)m->n_entries;
    m->entries[m->n_entries].key = key;
    m->entries[m->n_entries].value = value;
    m->n_entries++;
    m->size++;
    return 0;
}

static int
qp_symmap_del(qp_symmap *m, JSAtom key)
{
    uint32_t mask, i;
    if (m->index_size == 0)
        return 0;
    mask = m->index_size - 1;
    i = qp_symmap_slot(key, mask);
    for (;;) {
        int32_t ix = m->index[i];
        if (ix == QP_SYMMAP_EMPTY)
            return 0;
        if (ix >= 0 && m->entries[ix].key == key) {
            m->index[i] = QP_SYMMAP_DUMMY;
            m->entries[ix].key = JS_ATOM_NULL;
            m->size--;
            return 1;
        }
        i = (i + 1) & mask;
    }
}

static int
qp_symmap_next(const qp_symmap *m, Py_ssize_t *pos, JSAtom *key, long *value)
{
    Py_ssize_t i = *pos;
    while (i < (Py_ssize_t)m->n_entries) {
        if (m->entries[i].key != JS_ATOM_NULL) {
            if (key)
                *key = m->entries[i].key;
            if (value)
                *value = m->entries[i].value;
            *pos = i + 1;
            return 1;
        }
        i++;
    }
    *pos = i;
    return 0;
}

static int
qp_symmap_update(qp_symmap *dst, const qp_symmap *src)
{
    Py_ssize_t pos = 0;
    JSAtom k;
    long v;
    while (qp_symmap_next(src, &pos, &k, &v)) {
        if (qp_symmap_set(dst, k, v) < 0)
            return -1;
    }
    return 0;
}

static int
qp_atomlist_append(JSRuntime *rt, qp_atomlist *l, JSAtom a)
{
    if (l->len >= l->cap) {
        Py_ssize_t cap = l->cap ? l->cap * 2 : 8;
        JSAtom *p = js_realloc_rt(rt, l->items, sizeof(JSAtom) * cap);
        if (!p)
            return -1;
        l->items = p;
        l->cap = cap;
    }
    l->items[l->len++] = a;
    return 0;
}

static int
qp_stelist_reserve(JSRuntime *rt, qp_stelist *l, Py_ssize_t need)
{
    if (need > l->cap) {
        Py_ssize_t cap = l->cap ? l->cap : 4;
        PySTEntryObject **p;
        while (cap < need)
            cap *= 2;
        p = js_realloc_rt(rt, l->items, sizeof(PySTEntryObject *) * cap);
        if (!p)
            return -1;
        l->items = p;
        l->cap = cap;
    }
    return 0;
}

static int
qp_stelist_append(JSRuntime *rt, qp_stelist *l, PySTEntryObject *e)
{
    if (qp_stelist_reserve(rt, l, l->len + 1) < 0)
        return -1;
    l->items[l->len++] = e;
    return 0;
}

/* l[i:i+1] = src[:] (PyList_SetSlice) */
static int
qp_stelist_splice(JSRuntime *rt, qp_stelist *l, Py_ssize_t i, const qp_stelist *src)
{
    Py_ssize_t n = src->len;
    if (qp_stelist_reserve(rt, l, l->len - 1 + n) < 0)
        return -1;
    memmove(l->items + i + n, l->items + i + 1,
            sizeof(PySTEntryObject *) * (l->len - i - 1));
    if (n)
        memcpy(l->items + i, src->items, sizeof(PySTEntryObject *) * n);
    l->len += n - 1;
    return 0;
}

static void
qp_stelist_free(JSRuntime *rt, qp_stelist *l)
{
    if (l->items)
        js_free_rt(rt, l->items);
    l->items = NULL;
    l->len = l->cap = 0;
}

static int
qp_directives_append(JSRuntime *rt, qp_directives *l, JSAtom name, _Py_SourceLocation loc)
{
    if (l->len >= l->cap) {
        Py_ssize_t cap = l->cap ? l->cap * 2 : 4;
        qp_directive *p = js_realloc_rt(rt, l->items, sizeof(qp_directive) * cap);
        if (!p)
            return -1;
        l->items = p;
        l->cap = cap;
    }
    l->items[l->len].name = name;
    l->items[l->len].loc = loc;
    l->len++;
    return 0;
}

static inline uint32_t
qp_ptrmap_hash(const void *p)
{
    uint64_t x = (uint64_t)(uintptr_t)p;
    x ^= x >> 33;
    x *= 0xff51afd7ed558ccdULL;
    x ^= x >> 33;
    return (uint32_t)x;
}

static PySTEntryObject *
qp_ptrmap_get(const qp_ptrmap *m, void *key)
{
    uint32_t i;
    if (!m->slots)
        return NULL;
    i = qp_ptrmap_hash(key) & m->mask;
    while (m->slots[i].key) {
        if (m->slots[i].key == key)
            return m->slots[i].value;
        i = (i + 1) & m->mask;
    }
    return NULL;
}

/* Insert or replace (PyDict_SetItem semantics). */
static int
qp_ptrmap_set(JSRuntime *rt, qp_ptrmap *m, void *key, PySTEntryObject *value)
{
    uint32_t i;
    if (m->slots) {
        i = qp_ptrmap_hash(key) & m->mask;
        while (m->slots[i].key) {
            if (m->slots[i].key == key) {
                m->slots[i].value = value;
                return 0;
            }
            i = (i + 1) & m->mask;
        }
    }
    if (!m->slots || (m->count + 1) * 2 > m->mask + 1) {
        uint32_t new_size = m->slots ? (m->mask + 1) * 2 : 16, j;
        qp_ptrmap_entry *slots = js_mallocz_rt(rt, sizeof(qp_ptrmap_entry) * new_size);
        if (!slots)
            return -1;
        if (m->slots) {
            for (j = 0; j <= m->mask; j++) {
                if (m->slots[j].key) {
                    uint32_t k = qp_ptrmap_hash(m->slots[j].key) & (new_size - 1);
                    while (slots[k].key)
                        k = (k + 1) & (new_size - 1);
                    slots[k] = m->slots[j];
                }
            }
            js_free_rt(rt, m->slots);
        }
        m->slots = slots;
        m->mask = new_size - 1;
    }
    i = qp_ptrmap_hash(key) & m->mask;
    while (m->slots[i].key)
        i = (i + 1) & m->mask;
    m->slots[i].key = key;
    m->slots[i].value = value;
    m->count++;
    return 0;
}

static void
qp_ptrmap_free(JSRuntime *rt, qp_ptrmap *m)
{
    if (m->slots)
        js_free_rt(rt, m->slots);
    m->slots = NULL;
    m->mask = 0;
    m->count = 0;
}

/* ======================================================================
   Atoms and errors
   ====================================================================== */

/* String body of a string atom, NULL for JS_ATOM_NULL / integer atoms. */
static JSString *
qp_st_atom_str(JSContext *ctx, JSAtom a)
{
    if (a == JS_ATOM_NULL || __JS_AtomIsTaggedInt(a))
        return NULL;
    return ctx->rt->atom_array[a];
}

/* _PyUnicode_EqualToASCIIString for atoms, without allocating. */
static int
qp_st_atom_eq_ascii(JSContext *ctx, JSAtom a, const char *s)
{
    JSString *p = qp_st_atom_str(ctx, a);
    size_t n = strlen(s), i;
    if (!p || p->len != n)
        return 0;
    for (i = 0; i < n; i++) {
        if (string_get(p, (int)i) != (unsigned char)s[i])
            return 0;
    }
    return 1;
}

/* Index of the first `c` in `p`, or -1 (PyUnicode_FindChar). */
static int64_t
qp_st_find_char(const JSString *p, int c)
{
    uint32_t i;
    for (i = 0; i < p->len; i++) {
        if (string_get(p, (int)i) == c)
            return i;
    }
    return -1;
}

/* Report MemoryError, dropping the pending QuickJS exception if a JS API
   call failed. */
static void
qp_st_nomem(JSContext *ctx, qp_error *err, int clear_js_exception)
{
    if (clear_js_exception && ctx)
        JS_FreeValue(ctx, JS_GetException(ctx));
    qp_error_set(err, QP_ERR_MEMORY, 0, 0, 0, 0, "%s", "");
}

/* Format `fmt` (printf syntax) and report it with an explicit location. */
static void
qp_st_error_v(qp_error *err, qp_error_kind kind, int lineno, int offset,
              int end_lineno, int end_offset, const char *fmt, va_list ap)
{
    char sbuf[256];
    char *buf = sbuf;
    va_list ap2;
    int n;

    va_copy(ap2, ap);
    n = vsnprintf(sbuf, sizeof(sbuf), fmt, ap);
    if (n < 0) {
        n = 0;
        sbuf[0] = '\0';
    }
    if ((size_t)n >= sizeof(sbuf)) {
        buf = qp_sys_malloc((size_t)n + 1);
        if (!buf) {
            va_end(ap2);
            qp_st_nomem(NULL, err, 0);
            return;
        }
        vsnprintf(buf, (size_t)n + 1, fmt, ap2);
    }
    va_end(ap2);
    qp_error_set(err, kind, lineno, offset, end_lineno, end_offset, "%s", buf);
    if (buf != sbuf)
        qp_sys_free(buf);
}

static void
qp_st_error_at(qp_error *err, qp_error_kind kind, int lineno, int offset,
               int end_lineno, int end_offset, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    qp_st_error_v(err, kind, lineno, offset, end_lineno, end_offset, fmt, ap);
    va_end(ap);
}

static int
symtable_nomem(struct symtable *st)
{
    qp_st_nomem(st->st_ctx, st->st_err, 0);
    return 0;
}

/* PyErr_Format(PyExc_SyntaxError, fmt, ...) + SET_ERROR_LOCATION(loc).
   Always returns 0. */
static int
symtable_error(struct symtable *st, _Py_SourceLocation loc, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    qp_st_error_v(st->st_err, QP_ERR_SYNTAX, loc.lineno, loc.col_offset + 1,
                  loc.end_lineno, loc.end_col_offset + 1, fmt, ap);
    va_end(ap);
    return 0;
}

/* SyntaxError whose `fmt` takes the UTF-8 text of `name` as its %s. */
static int
symtable_error_name(struct symtable *st, _Py_SourceLocation loc, const char *fmt, JSAtom name)
{
    const char *s = JS_AtomToCStringLen(st->st_ctx, NULL, name);
    if (!s) {
        qp_st_nomem(st->st_ctx, st->st_err, 1);
        return 0;
    }
    symtable_error(st, loc, fmt, s);
    JS_FreeCString(st->st_ctx, s);
    return 0;
}

/* Take over one reference to `a` (a new reference): the symtable keeps
   exactly one reference per distinct atom, released by _PySymtable_Free.
   Returns 0, or -1 with MemoryError set (the reference is released). */
static int
symtable_adopt_atom(struct symtable *st, JSAtom a)
{
    if (qp_symmap_contains(&st->st_owned_atoms, a)) {
        JS_FreeAtom(st->st_ctx, a);
        return 0;
    }
    if (qp_symmap_set(&st->st_owned_atoms, a, 0) < 0) {
        JS_FreeAtom(st->st_ctx, a);
        symtable_nomem(st);
        return -1;
    }
    return 0;
}

/* Atom for a C string, owned by the symtable; JS_ATOM_NULL on OOM. */
static JSAtom
symtable_new_atom(struct symtable *st, const char *s, size_t len)
{
    JSAtom a = JS_NewAtomLen(st->st_ctx, s, len);
    if (a == JS_ATOM_NULL) {
        qp_st_nomem(st->st_ctx, st->st_err, 1);
        return JS_ATOM_NULL;
    }
    if (symtable_adopt_atom(st, a) < 0)
        return JS_ATOM_NULL;
    return a;
}

/* ======================================================================
   Name mangling
   ====================================================================== */

/* _Py_Mangle core.  Returns `ident` itself (*is_new = 0) when no mangling
   applies, a new atom reference "_" + privateobj.lstrip("_") + ident
   (*is_new = 1), or JS_ATOM_NULL with `err` set. */
static JSAtom
qp_mangle(JSContext *ctx, qp_error *err, JSAtom privateobj, JSAtom ident, int *is_new)
{
    /* Name mangling: __private becomes _classname__private.
       This is independent from how the name is used. */
    JSString *priv, *id;
    uint32_t nlen, plen, ipriv;
    StringBuffer b;
    JSValue v;
    JSAtom res;

    *is_new = 0;
    priv = qp_st_atom_str(ctx, privateobj);
    id = qp_st_atom_str(ctx, ident);
    if (priv == NULL || id == NULL || id->len < 2 ||
        string_get(id, 0) != '_' || string_get(id, 1) != '_') {
        return ident;
    }
    nlen = id->len;
    plen = priv->len;
    /* Don't mangle __id__ or names with dots.

       The only time a name with a dot can occur is when
       we are compiling an import statement that has a
       package name. */
    if ((string_get(id, nlen - 1) == '_' && string_get(id, nlen - 2) == '_') ||
        qp_st_find_char(id, '.') != -1) {
        return ident; /* Don't mangle __whatever__ */
    }
    /* Strip leading underscores from class name */
    ipriv = 0;
    while (ipriv < plen && string_get(priv, ipriv) == '_')
        ipriv++;
    if (ipriv == plen)
        return ident; /* Don't mangle if class is just underscores */

    if ((uint64_t)nlen + (plen - ipriv) + 1 > JS_STRING_LEN_MAX) {
        qp_error_set(err, QP_ERR_OVERFLOW, 0, 0, 0, 0, "%s",
                     "private identifier too large to be mangled");
        return JS_ATOM_NULL;
    }

    // ident = "_" + priv[ipriv:] + ident
    string_buffer_init(ctx, &b, 1 + nlen + (plen - ipriv));
    string_buffer_putc8(&b, '_');
    string_buffer_concat(&b, priv, ipriv, plen);
    string_buffer_concat(&b, id, 0, nlen);
    v = string_buffer_end(&b);
    if (JS_IsException(v)) {
        qp_st_nomem(ctx, err, 1);
        return JS_ATOM_NULL;
    }
    res = JS_NewAtomStr(ctx, JS_VALUE_GET_STRING(v));
    if (res == JS_ATOM_NULL) {
        qp_st_nomem(ctx, err, 1);
        return JS_ATOM_NULL;
    }
    *is_new = 1;
    return res;
}

static JSAtom
_Py_Mangle(JSAtom privateobj, JSAtom ident, JSContext *ctx, qp_error *err)
{
    int is_new;
    JSAtom res = qp_mangle(ctx, err, privateobj, ident, &is_new);
    if (res != JS_ATOM_NULL && !is_new)
        JS_DupAtom(ctx, res);
    return res;
}

/* Like _Py_MaybeMangle but the result is borrowed: either `name` or an
   atom now owned by the symtable.  JS_ATOM_NULL on error. */
static JSAtom
symtable_maybe_mangle(struct symtable *st, JSAtom privateobj,
                      PySTEntryObject *ste, JSAtom name)
{
    int is_new;
    JSAtom res;
    /* Special case for type parameter blocks around generic classes:
     * we want to mangle type parameter names (so a type param with a private
     * name can be used inside the class body), but we don't want to mangle
     * any other names that appear within the type parameter scope.
     */
    if (ste->ste_mangled_names != NULL &&
        !qp_symmap_contains(ste->ste_mangled_names, name)) {
        return name;
    }
    res = qp_mangle(st->st_ctx, st->st_err, privateobj, name, &is_new);
    if (res != JS_ATOM_NULL && is_new && symtable_adopt_atom(st, res) < 0)
        return JS_ATOM_NULL;
    return res;
}

static JSAtom
_Py_MaybeMangle(JSAtom privateobj, PySTEntryObject *ste, JSAtom name,
                JSContext *ctx, qp_error *err)
{
    if (ste->ste_mangled_names != NULL &&
        !qp_symmap_contains(ste->ste_mangled_names, name)) {
        return JS_DupAtom(ctx, name);
    }
    return _Py_Mangle(privateobj, name, ctx, err);
}

static int
_Py_IsPrivateName(JSContext *ctx, JSAtom ident)
{
    JSString *p = qp_st_atom_str(ctx, ident);
    uint32_t nlen;
    if (p == NULL)
        return 0;
    nlen = p->len;
    if (nlen < 3 || string_get(p, 0) != '_' || string_get(p, 1) != '_')
        return 0;
    if (string_get(p, nlen - 1) == '_' && string_get(p, nlen - 2) == '_')
        return 0; /* Don't mangle __whatever__ */
    return 1;
}

/* ======================================================================
   __future__ (Python/future.c)
   ====================================================================== */

/* Byte length PyUnicode_FromFormat("%.Ns") keeps of a UTF-8 string: at most
   `precision` bytes, minus an incomplete multi-byte sequence at the cut. */
static size_t
qp_st_utf8_precision(const char *s, size_t len, size_t precision)
{
    size_t n = len < precision ? len : precision;
    if (n == precision && n > 0) {
        size_t j = n, cont = 0;
        while (j > 0 && cont < 3 && ((unsigned char)s[j - 1] & 0xC0) == 0x80) {
            j--;
            cont++;
        }
        if (j > 0) {
            unsigned char c = (unsigned char)s[j - 1];
            size_t need = c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC0 ? 2 : 1;
            if (need > 1 && n - (j - 1) < need)
                n = j - 1;
        }
    }
    return n;
}

static int
future_check_features(_PyFutureFeatures *ff, stmt_ty s, const char *filename,
                      JSContext *ctx, qp_error *err)
{
    Py_ssize_t i;
    asdl_alias_seq *names = s->v.ImportFrom.names;

    (void)filename;
    for (i = 0; i < asdl_seq_LEN(names); i++) {
        alias_ty name = (alias_ty)asdl_seq_GET(names, i);
        size_t flen;
        const char *feature = JS_AtomToCStringLen(ctx, &flen, name->name);
        int ok = 1;
        if (!feature) {
            qp_st_nomem(ctx, err, 1);
            return 0;
        }
        if (strcmp(feature, FUTURE_NESTED_SCOPES) == 0) {
        } else if (strcmp(feature, FUTURE_GENERATORS) == 0) {
        } else if (strcmp(feature, FUTURE_DIVISION) == 0) {
        } else if (strcmp(feature, FUTURE_ABSOLUTE_IMPORT) == 0) {
        } else if (strcmp(feature, FUTURE_WITH_STATEMENT) == 0) {
        } else if (strcmp(feature, FUTURE_PRINT_FUNCTION) == 0) {
        } else if (strcmp(feature, FUTURE_UNICODE_LITERALS) == 0) {
        } else if (strcmp(feature, FUTURE_BARRY_AS_BDFL) == 0) {
            ff->ff_features |= CO_FUTURE_BARRY_AS_BDFL;
        } else if (strcmp(feature, FUTURE_GENERATOR_STOP) == 0) {
        } else if (strcmp(feature, FUTURE_ANNOTATIONS) == 0) {
            ff->ff_features |= CO_FUTURE_ANNOTATIONS;
        } else if (strcmp(feature, "braces") == 0) {
            qp_st_error_at(err, QP_ERR_SYNTAX, name->lineno, name->col_offset + 1,
                           name->end_lineno, name->end_col_offset + 1,
                           "%s", "not a chance");
            ok = 0;
        } else {
            /* UNDEFINED_FUTURE_FEATURE "future feature %.100s is not defined" */
            size_t n = qp_st_utf8_precision(feature, flen, 100);
            qp_st_error_at(err, QP_ERR_SYNTAX, name->lineno, name->col_offset + 1,
                           name->end_lineno, name->end_col_offset + 1,
                           "future feature %.*s is not defined", (int)n, feature);
            ok = 0;
        }
        JS_FreeCString(ctx, feature);
        if (!ok)
            return 0;
    }
    return 1;
}

static int
future_parse(_PyFutureFeatures *ff, mod_ty mod, const char *filename,
             JSContext *ctx, qp_error *err)
{
    asdl_stmt_seq *body;
    Py_ssize_t n, i;

    if (mod->kind == Module_kind)
        body = mod->v.Module.body;
    else if (mod->kind == Interactive_kind)
        body = mod->v.Interactive.body;
    else
        return 1;

    n = asdl_seq_LEN(body);
    if (n == 0)
        return 1;

    i = 0;
    if (_PyAST_GetDocString(body) != NULL)
        i++;

    for (; i < n; i++) {
        stmt_ty s = (stmt_ty)asdl_seq_GET(body, i);

        /* The only things that can precede a future statement
         *  are another future statement and a doc string.
         */

        if (s->kind == ImportFrom_kind && s->v.ImportFrom.level == 0) {
            JSAtom modname = s->v.ImportFrom.module;
            if (modname != JS_ATOM_NULL &&
                qp_st_atom_eq_ascii(ctx, modname, "__future__")) {
                if (!future_check_features(ff, s, filename, ctx, err))
                    return 0;
                ff->ff_location = SRC_LOCATION_FROM_AST(s);
            }
            else {
                return 1;
            }
        }
        else {
            return 1;
        }
    }
    return 1;
}

static int
_PyFuture_FromAST(mod_ty mod, const char *filename, _PyFutureFeatures *ff,
                  qp_arena *arena, qp_error *err)
{
    ff->ff_features = 0;
    ff->ff_location = (_Py_SourceLocation){-1, -1, -1, -1};

    if (!future_parse(ff, mod, filename, qp_arena_ctx(arena), err))
        return 0;
    return 1;
}

/* ======================================================================
   Symbol table
   ====================================================================== */

/* error strings */
#define GLOBAL_PARAM \
"name '%s' is parameter and global"

#define NONLOCAL_PARAM \
"name '%s' is parameter and nonlocal"

#define GLOBAL_AFTER_ASSIGN \
"name '%s' is assigned to before global declaration"

#define NONLOCAL_AFTER_ASSIGN \
"name '%s' is assigned to before nonlocal declaration"

#define GLOBAL_AFTER_USE \
"name '%s' is used prior to global declaration"

#define NONLOCAL_AFTER_USE \
"name '%s' is used prior to nonlocal declaration"

#define GLOBAL_ANNOT \
"annotated name '%s' can't be global"

#define NONLOCAL_ANNOT \
"annotated name '%s' can't be nonlocal"

#define IMPORT_STAR_WARNING "import * only allowed at module level"

#define NAMED_EXPR_COMP_IN_CLASS \
"assignment expression within a comprehension cannot be used in a class body"

#define NAMED_EXPR_COMP_IN_TYPEVAR_BOUND \
"assignment expression within a comprehension cannot be used in a TypeVar bound"

#define NAMED_EXPR_COMP_IN_TYPEALIAS \
"assignment expression within a comprehension cannot be used in a type alias"

#define NAMED_EXPR_COMP_IN_TYPEPARAM \
"assignment expression within a comprehension cannot be used within the definition of a generic"

#define NAMED_EXPR_COMP_CONFLICT \
"assignment expression cannot rebind comprehension iteration variable '%s'"

#define NAMED_EXPR_COMP_INNER_LOOP_CONFLICT \
"comprehension inner loop cannot rebind assignment expression target '%s'"

#define NAMED_EXPR_COMP_ITER_EXPR \
"assignment expression cannot be used in a comprehension iterable expression"

#define ANNOTATION_NOT_ALLOWED \
"%s cannot be used within an annotation"

#define EXPR_NOT_ALLOWED_IN_TYPE_VARIABLE \
"%s cannot be used within %s"

#define EXPR_NOT_ALLOWED_IN_TYPE_ALIAS \
"%s cannot be used within a type alias"

#define EXPR_NOT_ALLOWED_IN_TYPE_PARAMETERS \
"%s cannot be used within the definition of a generic"

#define DUPLICATE_TYPE_PARAM \
"duplicate type parameter '%s'"

#define ASYNC_WITH_OUTSIDE_ASYNC_FUNC \
"'async with' outside async function"

#define ASYNC_FOR_OUTSIDE_ASYNC_FUNC \
"'async for' outside async function"

#define DUPLICATE_PARAMETER \
"duplicate parameter '%s' in function definition"

#define LOCATION(x) SRC_LOCATION_FROM_AST(x)

#define IS_ASYNC_DEF(st) ((st)->st_cur->ste_type == FunctionBlock && (st)->st_cur->ste_coroutine)

static PySTEntryObject *
ste_new(struct symtable *st, JSAtom name, _Py_block_ty block,
        void *key, _Py_SourceLocation loc)
{
    JSRuntime *rt = st->st_ctx->rt;
    PySTEntryObject *ste = js_mallocz_rt(rt, sizeof(PySTEntryObject));
    if (ste == NULL) {
        symtable_nomem(st);
        return NULL;
    }
    if (qp_stelist_append(rt, &st->st_entries, ste) < 0) {
        js_free_rt(rt, ste);
        symtable_nomem(st);
        return NULL;
    }
    ste->ste_table = st;
    ste->ste_id = key;

    ste->ste_name = name;
    ste->ste_function_name = JS_ATOM_NULL;

    qp_symmap_init(&ste->ste_symbols, rt);
    ste->ste_mangled_names = NULL;

    ste->ste_type = block;
    ste->ste_scope_info = NULL;

    ste->ste_nested = 0;
    ste->ste_varargs = 0;
    ste->ste_varkeywords = 0;
    ste->ste_annotations_used = 0;
    ste->ste_loc = loc;

    if (st->st_cur != NULL &&
        (st->st_cur->ste_nested ||
         _PyST_IsFunctionLike(st->st_cur)))
        ste->ste_nested = 1;
    ste->ste_generator = 0;
    ste->ste_coroutine = 0;
    ste->ste_comprehension = NoComprehension;
    ste->ste_returns_value = 0;
    ste->ste_needs_class_closure = 0;
    ste->ste_comp_inlined = 0;
    ste->ste_comp_iter_target = 0;
    ste->ste_can_see_class_scope = 0;
    ste->ste_comp_iter_expr = 0;
    ste->ste_needs_classdict = 0;
    ste->ste_has_conditional_annotations = 0;
    ste->ste_in_conditional_block = 0;
    ste->ste_in_try_block = 0;
    ste->ste_in_unevaluated_annotation = 0;
    ste->ste_annotation_block = NULL;

    ste->ste_has_docstring = 0;

    ste->ste_method = 0;
    if (st->st_cur != NULL &&
        st->st_cur->ste_type == ClassBlock &&
        block == FunctionBlock) {
        ste->ste_method = 1;
    }

    if (qp_ptrmap_set(rt, &st->st_blocks, ste->ste_id, ste) < 0) {
        symtable_nomem(st);
        return NULL;
    }
    return ste;
}

/* ste_dealloc counterpart: release an entry's containers (the entry's atoms
   and its shared ste_mangled_names set belong to the symtable). */
static void
ste_free(JSRuntime *rt, PySTEntryObject *ste)
{
    qp_symmap_free(&ste->ste_symbols);
    if (ste->ste_varnames.items)
        js_free_rt(rt, ste->ste_varnames.items);
    qp_stelist_free(rt, &ste->ste_children);
    if (ste->ste_directives.items)
        js_free_rt(rt, ste->ste_directives.items);
    js_free_rt(rt, ste);
}

static int symtable_analyze(struct symtable *st);
static int symtable_enter_block(struct symtable *st, JSAtom name,
                                _Py_block_ty block, void *ast, _Py_SourceLocation loc);
static int symtable_exit_block(struct symtable *st);
static int symtable_visit_stmt(struct symtable *st, stmt_ty s);
static int symtable_visit_expr(struct symtable *st, expr_ty s);
static int symtable_visit_type_param(struct symtable *st, type_param_ty s);
static int symtable_visit_genexp(struct symtable *st, expr_ty s);
static int symtable_visit_listcomp(struct symtable *st, expr_ty s);
static int symtable_visit_setcomp(struct symtable *st, expr_ty s);
static int symtable_visit_dictcomp(struct symtable *st, expr_ty s);
static int symtable_visit_arguments(struct symtable *st, arguments_ty);
static int symtable_visit_excepthandler(struct symtable *st, excepthandler_ty);
static int symtable_visit_alias(struct symtable *st, alias_ty);
static int symtable_visit_comprehension(struct symtable *st, comprehension_ty);
static int symtable_visit_keyword(struct symtable *st, keyword_ty);
static int symtable_visit_params(struct symtable *st, asdl_arg_seq *args);
static int symtable_visit_annotation(struct symtable *st, expr_ty annotation, void *key);
static int symtable_visit_argannotations(struct symtable *st, asdl_arg_seq *args);
static int symtable_implicit_arg(struct symtable *st, int pos);
static int symtable_visit_annotations(struct symtable *st, stmt_ty, arguments_ty, expr_ty,
                                      struct _symtable_entry *parent_ste);
static int symtable_visit_withitem(struct symtable *st, withitem_ty item);
static int symtable_visit_match_case(struct symtable *st, match_case_ty m);
static int symtable_visit_pattern(struct symtable *st, pattern_ty s);
static int symtable_raise_if_annotation_block(struct symtable *st, const char *, expr_ty);
static int symtable_raise_if_not_coroutine(struct symtable *st, const char *msg, _Py_SourceLocation loc);
static int symtable_raise_if_comprehension_block(struct symtable *st, expr_ty);
static int symtable_add_def(struct symtable *st, JSAtom name, int flag, _Py_SourceLocation loc);

/* For debugging purposes only */
#if QP_DUMP_SYMTABLE
static void _dump_symtable(PySTEntryObject* ste, int indent)
{
    JSContext *ctx = ste->ste_table->st_ctx;
    char buf[ATOM_GET_STR_BUF_SIZE];
    const char *blocktype = "";
    switch (ste->ste_type) {
        case FunctionBlock: blocktype = "FunctionBlock"; break;
        case ClassBlock: blocktype = "ClassBlock"; break;
        case ModuleBlock: blocktype = "ModuleBlock"; break;
        case AnnotationBlock: blocktype = "AnnotationBlock"; break;
        case TypeVariableBlock: blocktype = "TypeVariableBlock"; break;
        case TypeAliasBlock: blocktype = "TypeAliasBlock"; break;
        case TypeParametersBlock: blocktype = "TypeParametersBlock"; break;
    }
    const char *comptype = "";
    switch (ste->ste_comprehension) {
        case ListComprehension: comptype = " ListComprehension"; break;
        case DictComprehension: comptype = " DictComprehension"; break;
        case SetComprehension: comptype = " SetComprehension"; break;
        case GeneratorExpression: comptype = " GeneratorExpression"; break;
        case NoComprehension: break;
    }
    printf("%*s=== Symtable for %s ===\n", indent, "",
           JS_AtomGetStr(ctx, buf, sizeof(buf), ste->ste_name));
    printf("%*s%s%s\n", indent, "", blocktype, comptype);
    printf("%*s%s%s%s%s%s%s%s%s%s%s%s\n", indent, "",
           ste->ste_nested ? " nested" : "",
           ste->ste_generator ? " generator" : "",
           ste->ste_coroutine ? " coroutine" : "",
           ste->ste_varargs ? " varargs" : "",
           ste->ste_varkeywords ? " varkeywords" : "",
           ste->ste_returns_value ? " returns_value" : "",
           ste->ste_needs_class_closure ? " needs_class_closure" : "",
           ste->ste_needs_classdict ? " needs_classdict" : "",
           ste->ste_comp_inlined ? " comp_inlined" : "",
           ste->ste_comp_iter_target ? " comp_iter_target" : "",
           ste->ste_can_see_class_scope ? " can_see_class_scope" : "");
    printf("%*slineno: %d col_offset: %d\n", indent, "",
           ste->ste_loc.lineno, ste->ste_loc.col_offset);
    printf("%*s--- Symbols ---\n", indent, "");
    Py_ssize_t pos = 0;
    JSAtom name;
    long flags;
    while (qp_symmap_next(&ste->ste_symbols, &pos, &name, &flags)) {
        int scope = SYMBOL_TO_SCOPE(flags);
        printf("%*s  %s: ", indent, "", JS_AtomGetStr(ctx, buf, sizeof(buf), name));
        if (flags & DEF_GLOBAL) printf(" DEF_GLOBAL");
        if (flags & DEF_LOCAL) printf(" DEF_LOCAL");
        if (flags & DEF_PARAM) printf(" DEF_PARAM");
        if (flags & DEF_NONLOCAL) printf(" DEF_NONLOCAL");
        if (flags & USE) printf(" USE");
        if (flags & DEF_FREE_CLASS) printf(" DEF_FREE_CLASS");
        if (flags & DEF_IMPORT) printf(" DEF_IMPORT");
        if (flags & DEF_ANNOT) printf(" DEF_ANNOT");
        if (flags & DEF_COMP_ITER) printf(" DEF_COMP_ITER");
        if (flags & DEF_TYPE_PARAM) printf(" DEF_TYPE_PARAM");
        if (flags & DEF_COMP_CELL) printf(" DEF_COMP_CELL");
        switch (scope) {
            case LOCAL: printf(" LOCAL"); break;
            case GLOBAL_EXPLICIT: printf(" GLOBAL_EXPLICIT"); break;
            case GLOBAL_IMPLICIT: printf(" GLOBAL_IMPLICIT"); break;
            case FREE: printf(" FREE"); break;
            case CELL: printf(" CELL"); break;
        }
        printf("\n");
    }
    printf("%*s--- Children ---\n", indent, "");
    for (Py_ssize_t i = 0; i < ste->ste_children.len; i++) {
        _dump_symtable(ste->ste_children.items[i], indent + 2);
    }
}

static void dump_symtable(PySTEntryObject* ste)
{
    _dump_symtable(ste, 0);
}
#endif

/* Create the symtable's own atoms (CPython's _Py_ID/_Py_STR statics). */
static int
symtable_init_ids(struct symtable *st)
{
    static const struct {
        size_t offset;
        const char *str;
    } ids[] = {
        { offsetof(qp_st_ids, top), "top" },
        { offsetof(qp_st_ids, dot_format), ".format" },
        { offsetof(qp_st_ids, dot_type_params), ".type_params" },
        { offsetof(qp_st_ids, dot_generic_base), ".generic_base" },
        { offsetof(qp_st_ids, dot_defaults), ".defaults" },
        { offsetof(qp_st_ids, dot_kwdefaults), ".kwdefaults" },
        { offsetof(qp_st_ids, dunder_class), "__class__" },
        { offsetof(qp_st_ids, dunder_classdict), "__classdict__" },
        { offsetof(qp_st_ids, dunder_conditional_annotations), "__conditional_annotations__" },
        { offsetof(qp_st_ids, dunder_type_params), "__type_params__" },
        { offsetof(qp_st_ids, dunder_annotate), "__annotate__" },
        { offsetof(qp_st_ids, anon_lambda), "<lambda>" },
        { offsetof(qp_st_ids, anon_genexpr), "<genexpr>" },
        { offsetof(qp_st_ids, anon_listcomp), "<listcomp>" },
        { offsetof(qp_st_ids, anon_setcomp), "<setcomp>" },
        { offsetof(qp_st_ids, anon_dictcomp), "<dictcomp>" },
    };
    size_t i;
    for (i = 0; i < sizeof(ids) / sizeof(ids[0]); i++) {
        JSAtom a = symtable_new_atom(st, ids[i].str, strlen(ids[i].str));
        if (a == JS_ATOM_NULL)
            return 0;
        *(JSAtom *)((char *)&st->st_ids + ids[i].offset) = a;
    }
    return 1;
}

static struct symtable *
symtable_new(qp_arena *arena, qp_error *err)
{
    JSContext *ctx = qp_arena_ctx(arena);
    struct symtable *st;

    st = js_mallocz_rt(ctx->rt, sizeof(struct symtable));
    if (st == NULL) {
        qp_st_nomem(ctx, err, 0);
        return NULL;
    }
    st->st_ctx = ctx;
    st->st_arena = arena;
    st->st_err = err;
    st->st_filename = NULL;
    st->st_cur = NULL;
    st->st_private = JS_ATOM_NULL;
    qp_symmap_init(&st->st_owned_atoms, ctx->rt);
    if (!symtable_init_ids(st)) {
        _PySymtable_Free(st);
        return NULL;
    }
    return st;
}

/* Allocate an (empty) ste_mangled_names set owned by the symtable. */
static qp_symmap *
symtable_new_set(struct symtable *st)
{
    JSRuntime *rt = st->st_ctx->rt;
    qp_symmap *set;
    if (st->st_nsets >= st->st_sets_cap) {
        Py_ssize_t cap = st->st_sets_cap ? st->st_sets_cap * 2 : 4;
        qp_symmap **p = js_realloc_rt(rt, st->st_sets, sizeof(qp_symmap *) * cap);
        if (!p) {
            symtable_nomem(st);
            return NULL;
        }
        st->st_sets = p;
        st->st_sets_cap = cap;
    }
    set = js_malloc_rt(rt, sizeof(qp_symmap));
    if (!set) {
        symtable_nomem(st);
        return NULL;
    }
    qp_symmap_init(set, rt);
    st->st_sets[st->st_nsets++] = set;
    return set;
}

static struct symtable *
_PySymtable_Build(mod_ty mod, const char *filename, _PyFutureFeatures *future,
                  qp_arena *arena, qp_error *err)
{
    struct symtable *st = symtable_new(arena, err);
    asdl_stmt_seq *seq;
    Py_ssize_t i;

    if (st == NULL)
        return NULL;
    st->st_filename = filename;
    st->st_future = future;

    /* Make the initial symbol information gathering pass */

    _Py_SourceLocation loc0 = {0, 0, 0, 0};
    if (!symtable_enter_block(st, st->st_ids.top, ModuleBlock, (void *)mod, loc0)) {
        _PySymtable_Free(st);
        return NULL;
    }

    st->st_top = st->st_cur;
    switch (mod->kind) {
    case Module_kind:
        seq = mod->v.Module.body;
        if (_PyAST_GetDocString(seq)) {
            st->st_cur->ste_has_docstring = 1;
        }
        for (i = 0; i < asdl_seq_LEN(seq); i++)
            if (!symtable_visit_stmt(st,
                        (stmt_ty)asdl_seq_GET(seq, i)))
                goto error;
        break;
    case Expression_kind:
        if (!symtable_visit_expr(st, mod->v.Expression.body))
            goto error;
        break;
    case Interactive_kind:
        seq = mod->v.Interactive.body;
        for (i = 0; i < asdl_seq_LEN(seq); i++)
            if (!symtable_visit_stmt(st,
                        (stmt_ty)asdl_seq_GET(seq, i)))
                goto error;
        break;
    case FunctionType_kind:
        /* RuntimeError in CPython */
        qp_error_set(st->st_err, QP_ERR_VALUE, 0, 0, 0, 0, "%s",
                     "this compiler does not handle FunctionTypes");
        goto error;
    }
    if (!symtable_exit_block(st)) {
        _PySymtable_Free(st);
        return NULL;
    }
    /* Make the second symbol analysis pass */
    if (symtable_analyze(st)) {
#if QP_DUMP_SYMTABLE
        dump_symtable(st->st_top);
#endif
        return st;
    }
    _PySymtable_Free(st);
    return NULL;
 error:
    (void) symtable_exit_block(st);
    _PySymtable_Free(st);
    return NULL;
}

static void
_PySymtable_Free(struct symtable *st)
{
    JSRuntime *rt;
    Py_ssize_t i, pos;
    JSAtom a;

    if (st == NULL)
        return;
    rt = st->st_ctx->rt;
    for (i = 0; i < st->st_entries.len; i++)
        ste_free(rt, st->st_entries.items[i]);
    qp_stelist_free(rt, &st->st_entries);
    qp_stelist_free(rt, &st->st_stack);
    qp_ptrmap_free(rt, &st->st_blocks);
    for (i = 0; i < st->st_nsets; i++) {
        qp_symmap_free(st->st_sets[i]);
        js_free_rt(rt, st->st_sets[i]);
    }
    if (st->st_sets)
        js_free_rt(rt, st->st_sets);
    pos = 0;
    while (qp_symmap_next(&st->st_owned_atoms, &pos, &a, NULL))
        JS_FreeAtom(st->st_ctx, a);
    qp_symmap_free(&st->st_owned_atoms);
    js_free_rt(rt, st);
}

static PySTEntryObject *
_PySymtable_Lookup(struct symtable *st, void *key)
{
    PySTEntryObject *v = qp_ptrmap_get(&st->st_blocks, key);
    if (v == NULL) {
        /* KeyError in CPython */
        qp_error_set(st->st_err, QP_ERR_VALUE, 0, 0, 0, 0, "%s",
                     "unknown symbol table entry");
    }
    return v;
}

static int
_PySymtable_LookupOptional(struct symtable *st, void *key,
                           PySTEntryObject **out)
{
    *out = qp_ptrmap_get(&st->st_blocks, key);
    return *out != NULL;
}

static long
_PyST_GetSymbol(PySTEntryObject *ste, JSAtom name)
{
    long v;
    if (!qp_symmap_lookup(&ste->ste_symbols, name, &v))
        return 0;
    return v;
}

static int
_PyST_GetScope(PySTEntryObject *ste, JSAtom name)
{
    long symbol = _PyST_GetSymbol(ste, name);
    return SYMBOL_TO_SCOPE(symbol);
}

static int
_PyST_IsFunctionLike(PySTEntryObject *ste)
{
    return ste->ste_type == FunctionBlock
        || ste->ste_type == AnnotationBlock
        || ste->ste_type == TypeVariableBlock
        || ste->ste_type == TypeAliasBlock
        || ste->ste_type == TypeParametersBlock;
}

/* Raise the SyntaxError `fmt` (taking the name as %s, or no conversion)
   at the location of the global/nonlocal directive for `name`. */
static int
error_at_directive(PySTEntryObject *ste, JSAtom name, const char *fmt)
{
    Py_ssize_t i;
    for (i = 0; i < ste->ste_directives.len; i++) {
        qp_directive *data = &ste->ste_directives.items[i];
        if (data->name == name) {
            return symtable_error_name(ste->ste_table, data->loc, fmt, name);
        }
    }
    /* RuntimeError in CPython */
    qp_error_set(ste->ste_table->st_err, QP_ERR_VALUE, 0, 0, 0, 0, "%s",
                 "BUG: internal directive bookkeeping broken");
    return 0;
}


/* Analyze raw symbol information to determine scope of each name.

   The next several functions are helpers for symtable_analyze(),
   which determines whether a name is local, global, or free.  In addition,
   it determines which local variables are cell variables; they provide
   bindings that are used for free variables in enclosed blocks.

   There are also two kinds of global variables, implicit and explicit.  An
   explicit global is declared with the global statement.  An implicit
   global is a free variable for which the compiler has found no binding
   in an enclosing function scope.  The implicit global is either a global
   or a builtin.  Python's module and class blocks use the xxx_NAME opcodes
   to handle these names to implement slightly odd semantics.  In such a
   block, the name is treated as global until it is assigned to; then it
   is treated as a local.

   The symbol table requires two passes to determine the scope of each name.
   The first pass collects raw facts from the AST via the symtable_visit_*
   functions: the name is a parameter here, the name is used but not defined
   here, etc.  The second pass analyzes these facts during a pass over the
   PySTEntryObjects created during pass 1.

   When a function is entered during the second pass, the parent passes
   the set of all name bindings visible to its children.  These bindings
   are used to determine if non-local variables are free or implicit globals.
   Names which are explicitly declared nonlocal must exist in this set of
   visible names - if they do not, a syntax error is raised. After doing
   the local analysis, it analyzes each of its child blocks using an
   updated set of name bindings.

   The children update the free variable set.  If a local variable is added to
   the free variable set by the child, the variable is marked as a cell.  The
   function object being defined must provide runtime storage for the variable
   that may outlive the function's frame.  Cell variables are removed from the
   free set before the analyze function returns to its parent.

   During analysis, the names are:
      symbols: dict mapping from symbol names to flag values (including offset scope values)
      scopes: dict mapping from symbol names to scope values (no offset)
      local: set of all symbol names local to the current scope
      bound: set of all symbol names local to a containing function scope
      free: set of all symbol names referenced but not bound in child scopes
      global: set of all symbol names explicitly declared as global
*/

static int
analysis_nomem(PySTEntryObject *ste)
{
    return symtable_nomem(ste->ste_table);
}

#define SET_SCOPE(DICT, NAME, I) \
    do { \
        if (qp_symmap_set((DICT), (NAME), (I)) < 0) \
            return analysis_nomem(ste); \
    } while(0)

#define SET_ADD(SET, NAME) \
    do { \
        if (qp_symmap_set((SET), (NAME), 0) < 0) \
            return analysis_nomem(ste); \
    } while(0)

/* Decide on scope of name, given flags.

   The namespace dictionaries may be modified to record information
   about the new name.  For example, a new global will add an entry to
   global.  A name that was global can be changed to local.
*/

static int
analyze_name(PySTEntryObject *ste, qp_symmap *scopes, JSAtom name, long flags,
             qp_symmap *bound, qp_symmap *local, qp_symmap *free,
             qp_symmap *global, qp_symmap *type_params, PySTEntryObject *class_entry)
{
    if (flags & DEF_GLOBAL) {
        if (flags & DEF_NONLOCAL) {
            return error_at_directive(ste, name,
                                      "name '%s' is nonlocal and global");
        }
        SET_SCOPE(scopes, name, GLOBAL_EXPLICIT);
        SET_ADD(global, name);
        if (bound)
            qp_symmap_del(bound, name);
        return 1;
    }
    if (flags & DEF_NONLOCAL) {
        if (!bound) {
            return error_at_directive(ste, name,
                                      "nonlocal declaration not allowed at module level");
        }
        if (!qp_symmap_contains(bound, name)) {
            return error_at_directive(ste, name,
                                      "no binding for nonlocal '%s' found");
        }
        if (qp_symmap_contains(type_params, name)) {
            return error_at_directive(ste, name,
                                      "nonlocal binding not allowed for type parameter '%s'");
        }
        SET_SCOPE(scopes, name, FREE);
        SET_ADD(free, name);
        return 1;
    }
    if (flags & DEF_BOUND) {
        SET_SCOPE(scopes, name, LOCAL);
        SET_ADD(local, name);
        qp_symmap_del(global, name);
        if (flags & DEF_TYPE_PARAM) {
            SET_ADD(type_params, name);
        }
        else {
            qp_symmap_del(type_params, name);
        }
        return 1;
    }
    // If we were passed class_entry (i.e., we're in an ste_can_see_class_scope scope)
    // and the bound name is in that set, then the name is potentially bound both by
    // the immediately enclosing class namespace, and also by an outer function namespace.
    // In that case, we want the runtime name resolution to look at only the class
    // namespace and the globals (not the namespace providing the bound).
    // Similarly, if the name is explicitly global in the class namespace (through the
    // global statement), we want to also treat it as a global in this scope.
    if (class_entry != NULL) {
        long class_flags = _PyST_GetSymbol(class_entry, name);
        if (class_flags & DEF_GLOBAL) {
            SET_SCOPE(scopes, name, GLOBAL_EXPLICIT);
            return 1;
        }
        else if ((class_flags & DEF_BOUND) && !(class_flags & DEF_NONLOCAL)) {
            SET_SCOPE(scopes, name, GLOBAL_IMPLICIT);
            return 1;
        }
    }
    /* If an enclosing block has a binding for this name, it
       is a free variable rather than a global variable.
       Note that having a non-NULL bound implies that the block
       is nested.
    */
    if (bound) {
        if (qp_symmap_contains(bound, name)) {
            SET_SCOPE(scopes, name, FREE);
            SET_ADD(free, name);
            return 1;
        }
    }
    /* If a parent has a global statement, then call it global
       explicit?  It could also be global implicit.
     */
    if (global) {
        if (qp_symmap_contains(global, name)) {
            SET_SCOPE(scopes, name, GLOBAL_IMPLICIT);
            return 1;
        }
    }
    SET_SCOPE(scopes, name, GLOBAL_IMPLICIT);
    return 1;
}

static int
is_free_in_any_child(PySTEntryObject *entry, JSAtom key)
{
    for (Py_ssize_t i = 0; i < entry->ste_children.len; i++) {
        PySTEntryObject *child_ste = entry->ste_children.items[i];
        long scope = _PyST_GetScope(child_ste, key);
        if (scope == FREE) {
            return 1;
        }
    }
    return 0;
}

static int
inline_comprehension(PySTEntryObject *ste, PySTEntryObject *comp,
                     qp_symmap *scopes, qp_symmap *comp_free,
                     qp_symmap *inlined_cells)
{
    qp_st_ids *ids = &ste->ste_table->st_ids;
    JSAtom k;
    long comp_flags;
    Py_ssize_t pos = 0;
    int remove_dunder_class = 0;
    int remove_dunder_classdict = 0;
    int remove_dunder_cond_annotations = 0;

    while (qp_symmap_next(&comp->ste_symbols, &pos, &k, &comp_flags)) {
        // skip comprehension parameter
        if (comp_flags & DEF_PARAM) {
            continue;
        }
        int scope = SYMBOL_TO_SCOPE(comp_flags);
        int only_flags = comp_flags & ((1 << SCOPE_OFFSET) - 1);
        if (scope == CELL || only_flags & DEF_COMP_CELL) {
            SET_ADD(inlined_cells, k);
        }
        long flags = 0;
        int existing = qp_symmap_lookup(&ste->ste_symbols, k, &flags);
        // __class__, __classdict__ and __conditional_annotations__ are
        // not allowed to be free through a class scope (see
        // drop_class_free) unless children scopes need it
        if (scope == FREE && ste->ste_type == ClassBlock &&
                (k == ids->dunder_class ||
                 k == ids->dunder_classdict ||
                 k == ids->dunder_conditional_annotations)) {
            scope = GLOBAL_IMPLICIT;
            if (!is_free_in_any_child(comp, k)) {
                qp_symmap_del(comp_free, k);
            }
            if (k == ids->dunder_class) {
                remove_dunder_class = 1;
            }
            else if (k == ids->dunder_conditional_annotations) {
                remove_dunder_cond_annotations = 1;
            }
            else {
                remove_dunder_classdict = 1;
            }
        }
        if (!existing) {
            // name does not exist in scope, copy from comprehension
            assert(scope != FREE || qp_symmap_contains(comp_free, k));
            if (qp_symmap_set(&ste->ste_symbols, k, only_flags) < 0) {
                return analysis_nomem(ste);
            }
            SET_SCOPE(scopes, k, scope);
        }
        else {
            // Propagate uses of enclosing bindings, but not class locals,
            // which are not visible to comprehensions.
            if (scope == FREE && ste->ste_type != ClassBlock &&
                    (comp_flags & USE) && !(flags & USE)) {
                if (qp_symmap_set(&ste->ste_symbols, k, flags | USE) < 0) {
                    return analysis_nomem(ste);
                }
            }
            if ((flags & DEF_BOUND) && ste->ste_type != ClassBlock) {
                // free vars in comprehension that are locals in outer scope can
                // now simply be locals, unless they are free in comp children,
                // or if the outer scope is a class block
                if (!is_free_in_any_child(comp, k)) {
                    qp_symmap_del(comp_free, k);
                }
            }
        }
    }
    if (remove_dunder_class) {
        qp_symmap_del(&comp->ste_symbols, ids->dunder_class);
    }
    if (remove_dunder_classdict) {
        qp_symmap_del(&comp->ste_symbols, ids->dunder_classdict);
    }
    if (remove_dunder_cond_annotations) {
        qp_symmap_del(&comp->ste_symbols, ids->dunder_conditional_annotations);
    }
    return 1;
}

#undef SET_SCOPE
#undef SET_ADD

/* If a name is defined in free and also in locals, then this block
   provides the binding for the free variable.  The name should be
   marked CELL in this block and removed from the free list.

   Note that the current block's free variables are included in free.
   That's safe because no name can be free and local in the same scope.
*/

static int
analyze_cells(qp_symmap *scopes, qp_symmap *free, qp_symmap *inlined_cells)
{
    JSAtom name;
    long scope;
    Py_ssize_t pos = 0;

    while (qp_symmap_next(scopes, &pos, &name, &scope)) {
        if (scope != LOCAL)
            continue;
        if (!qp_symmap_contains(free, name)) {
            if (!qp_symmap_contains(inlined_cells, name)) {
                continue;
            }
        }
        /* Replace LOCAL with CELL for this name, and remove
           from free. It is safe to replace the value of name
           in the dict, because it will not cause a resize.
         */
        if (qp_symmap_set(scopes, name, CELL) < 0)
            return 0;
        qp_symmap_del(free, name);
    }
    return 1;
}

static int
drop_class_free(PySTEntryObject *ste, qp_symmap *free)
{
    qp_st_ids *ids = &ste->ste_table->st_ids;
    if (qp_symmap_del(free, ids->dunder_class))
        ste->ste_needs_class_closure = 1;
    if (qp_symmap_del(free, ids->dunder_classdict))
        ste->ste_needs_classdict = 1;
    if (qp_symmap_del(free, ids->dunder_conditional_annotations)) {
        ste->ste_has_conditional_annotations = 1;
    }
    return 1;
}

/* Enter the final scope information into the ste_symbols dict.
 *
 * All arguments are dicts.  Modifies symbols, others are read-only.
*/
static int
update_symbols(PySTEntryObject *ste, qp_symmap *symbols, qp_symmap *scopes,
               qp_symmap *bound, qp_symmap *free,
               qp_symmap *inlined_cells, int classflag)
{
    JSAtom name;
    long flags, scope;
    Py_ssize_t pos = 0;

    /* Update scope information for all symbols in this scope */
    while (qp_symmap_next(symbols, &pos, &name, &flags)) {
        if (qp_symmap_contains(inlined_cells, name)) {
            flags |= DEF_COMP_CELL;
        }
        if (!qp_symmap_lookup(scopes, name, &scope)) {
            /* KeyError in CPython */
            const char *s = JS_AtomToCStringLen(ste->ste_table->st_ctx, NULL, name);
            if (!s) {
                qp_st_nomem(ste->ste_table->st_ctx, ste->ste_table->st_err, 1);
                return 0;
            }
            qp_error_set(ste->ste_table->st_err, QP_ERR_VALUE, 0, 0, 0, 0, "'%s'", s);
            JS_FreeCString(ste->ste_table->st_ctx, s);
            return 0;
        }
        flags |= (scope << SCOPE_OFFSET);
        if (qp_symmap_set(symbols, name, flags) < 0)
            return analysis_nomem(ste);
    }

    /* Record not yet resolved free variables from children (if any) */
    pos = 0;
    while (qp_symmap_next(free, &pos, &name, NULL)) {
        long v;
        /* Handle symbol that already exists in this scope */
        if (qp_symmap_lookup(symbols, name, &v)) {
            /* Handle a free variable in a method of
               the class that has the same name as a local
               or global in the class scope.
            */
            if  (classflag) {
                if (qp_symmap_set(symbols, name, v | DEF_FREE_CLASS) < 0)
                    return analysis_nomem(ste);
            }
            /* It's a cell, or already free in this scope */
            continue;
        }
        /* Handle global symbol */
        if (bound && !qp_symmap_contains(bound, name)) {
            continue;       /* it's a global */
        }
        /* Propagate new free symbol up the lexical stack */
        if (qp_symmap_set(symbols, name, FREE << SCOPE_OFFSET) < 0)
            return analysis_nomem(ste);
    }
    return 1;
}

/* Make final symbol table decisions for block of ste.

   Arguments:
   ste -- current symtable entry (input/output)
   bound -- set of variables bound in enclosing scopes (input).  bound
       is NULL for module blocks.
   free -- set of free variables in enclosed scopes (output)
   globals -- set of declared global variables in enclosing scopes (input)

   The implementation uses two mutually recursive functions,
   analyze_block() and analyze_child_block().  analyze_block() is
   responsible for analyzing the individual names defined in a block.
   analyze_child_block() prepares temporary namespace dictionaries
   used to evaluated nested blocks.

   The two functions exist because a child block should see the name
   bindings of its enclosing blocks, but those bindings should not
   propagate back to a parent block.
*/

static int
analyze_child_block(PySTEntryObject *entry, qp_symmap *bound, qp_symmap *free,
                    qp_symmap *global, qp_symmap *type_params,
                    PySTEntryObject *class_entry, qp_symmap *child_free);

static int
analyze_block(PySTEntryObject *ste, qp_symmap *bound, qp_symmap *free,
              qp_symmap *global, qp_symmap *type_params,
              PySTEntryObject *class_entry)
{
    struct symtable *st = ste->ste_table;
    JSRuntime *rt = st->st_ctx->rt;
    qp_symmap local, scopes, newbound, newglobal, newfree, inlined_cells;
    JSAtom name;
    long flags;
    int success = 0;
    Py_ssize_t i, pos = 0;

    /* CPython relies on its large C stack here; QuickJS stacks can be
       smaller, so guard the mutual recursion. */
    if (js_check_stack_overflow(rt, 0)) {
        qp_error_set(st->st_err, QP_ERR_RECURSION, 0, 0, 0, 0, "%s",
                     "maximum recursion depth exceeded during compilation");
        return 0;
    }

    qp_symmap_init(&local, rt);  /* collect new names bound in block */
    qp_symmap_init(&scopes, rt);  /* collect scopes defined for each name */

    /* Allocate new global, bound and free variable sets.  These
       sets hold the names visible in nested blocks.  For
       ClassBlocks, the bound and global names are initialized
       before analyzing names, because class bindings aren't
       visible in methods.  For other blocks, they are initialized
       after names are analyzed.
     */
    qp_symmap_init(&newglobal, rt);
    qp_symmap_init(&newfree, rt);
    qp_symmap_init(&newbound, rt);
    qp_symmap_init(&inlined_cells, rt);

    /* Class namespace has no effect on names visible in
       nested functions, so populate the global and bound
       sets to be passed to child blocks before analyzing
       this one.
     */
    if (ste->ste_type == ClassBlock) {
        /* Pass down known globals */
        if (qp_symmap_update(&newglobal, global) < 0)
            goto nomem;
        /* Pass down previously bound symbols */
        if (bound) {
            if (qp_symmap_update(&newbound, bound) < 0)
                goto nomem;
        }
    }

    while (qp_symmap_next(&ste->ste_symbols, &pos, &name, &flags)) {
        if (!analyze_name(ste, &scopes, name, flags,
                          bound, &local, free, global, type_params, class_entry))
            goto error;
    }

    /* Populate global and bound sets to be passed to children. */
    if (ste->ste_type != ClassBlock) {
        /* Add function locals to bound set */
        if (_PyST_IsFunctionLike(ste)) {
            if (qp_symmap_update(&newbound, &local) < 0)
                goto nomem;
        }
        /* Pass down previously bound symbols */
        if (bound) {
            if (qp_symmap_update(&newbound, bound) < 0)
                goto nomem;
        }
        /* Pass down known globals */
        if (qp_symmap_update(&newglobal, global) < 0)
            goto nomem;
    }
    else {
        /* Special-case __class__ and __classdict__ */
        if (qp_symmap_set(&newbound, st->st_ids.dunder_class, 0) < 0)
            goto nomem;
        if (qp_symmap_set(&newbound, st->st_ids.dunder_classdict, 0) < 0)
            goto nomem;
        if (qp_symmap_set(&newbound, st->st_ids.dunder_conditional_annotations, 0) < 0)
            goto nomem;
    }

    /* Recursively call analyze_child_block() on each child block.

       newbound, newglobal now contain the names visible in
       nested blocks.  The free variables in the children will
       be added to newfree.
    */
    for (i = 0; i < ste->ste_children.len; ++i) {
        qp_symmap child_free;
        PySTEntryObject *entry = ste->ste_children.items[i];

        PySTEntryObject *new_class_entry = NULL;
        if (entry->ste_can_see_class_scope) {
            if (ste->ste_type == ClassBlock) {
                new_class_entry = ste;
            }
            else if (class_entry) {
                new_class_entry = class_entry;
            }
        }

        // we inline all non-generator-expression comprehensions,
        // except those in annotation scopes that are nested in classes
        int inline_comp =
            entry->ste_comprehension &&
            !entry->ste_generator &&
            !ste->ste_can_see_class_scope;

        if (!analyze_child_block(entry, &newbound, &newfree, &newglobal,
                                 type_params, new_class_entry, &child_free))
        {
            goto error;
        }
        if (inline_comp) {
            if (!inline_comprehension(ste, entry, &scopes, &child_free, &inlined_cells)) {
                qp_symmap_free(&child_free);
                goto error;
            }
            entry->ste_comp_inlined = 1;
        }
        if (qp_symmap_update(&newfree, &child_free) < 0) {
            qp_symmap_free(&child_free);
            goto nomem;
        }
        qp_symmap_free(&child_free);
    }

    /* Splice children of inlined comprehensions into our children list */
    for (i = ste->ste_children.len - 1; i >= 0; --i) {
        PySTEntryObject *entry = ste->ste_children.items[i];
        if (entry->ste_comp_inlined &&
            qp_stelist_splice(rt, &ste->ste_children, i,
                              &entry->ste_children) < 0)
        {
            goto nomem;
        }
    }

    /* Check if any local variables must be converted to cell variables */
    if (_PyST_IsFunctionLike(ste)) {
        if (!analyze_cells(&scopes, &newfree, &inlined_cells))
            goto nomem;
    }
    else if (ste->ste_type == ClassBlock && !drop_class_free(ste, &newfree))
        goto error;
    /* Records the results of the analysis in the symbol table entry */
    if (!update_symbols(ste, &ste->ste_symbols, &scopes, bound, &newfree, &inlined_cells,
                        (ste->ste_type == ClassBlock) || ste->ste_can_see_class_scope))
        goto error;

    if (qp_symmap_update(free, &newfree) < 0)
        goto nomem;
    success = 1;
    goto error;
 nomem:
    symtable_nomem(st);
 error:
    qp_symmap_free(&scopes);
    qp_symmap_free(&local);
    qp_symmap_free(&newbound);
    qp_symmap_free(&newglobal);
    qp_symmap_free(&newfree);
    qp_symmap_free(&inlined_cells);
    return success;
}

/* Analyze `entry` with private copies of the parent's sets.  On success
   *child_free is initialised with the child's free set (caller frees). */
static int
analyze_child_block(PySTEntryObject *entry, qp_symmap *bound, qp_symmap *free,
                    qp_symmap *global, qp_symmap *type_params,
                    PySTEntryObject *class_entry, qp_symmap *child_free)
{
    JSRuntime *rt = entry->ste_table->st_ctx->rt;
    qp_symmap temp_bound, temp_global, temp_free, temp_type_params;

    /* Copy the bound/global/free sets.

       These sets are used by all blocks enclosed by the
       current block.  The analyze_block() call modifies these
       sets.

    */
    qp_symmap_init(&temp_bound, rt);
    qp_symmap_init(&temp_free, rt);
    qp_symmap_init(&temp_global, rt);
    qp_symmap_init(&temp_type_params, rt);
    if (qp_symmap_update(&temp_bound, bound) < 0 ||
        qp_symmap_update(&temp_free, free) < 0 ||
        qp_symmap_update(&temp_global, global) < 0 ||
        qp_symmap_update(&temp_type_params, type_params) < 0) {
        symtable_nomem(entry->ste_table);
        goto error;
    }

    if (!analyze_block(entry, &temp_bound, &temp_free, &temp_global,
                       &temp_type_params, class_entry))
        goto error;
    *child_free = temp_free;
    qp_symmap_free(&temp_bound);
    qp_symmap_free(&temp_global);
    qp_symmap_free(&temp_type_params);
    return 1;
 error:
    qp_symmap_free(&temp_bound);
    qp_symmap_free(&temp_free);
    qp_symmap_free(&temp_global);
    qp_symmap_free(&temp_type_params);
    return 0;
}

static int
symtable_analyze(struct symtable *st)
{
    JSRuntime *rt = st->st_ctx->rt;
    qp_symmap free, global, type_params;
    int r;

    qp_symmap_init(&free, rt);
    qp_symmap_init(&global, rt);
    qp_symmap_init(&type_params, rt);
    r = analyze_block(st->st_top, NULL, &free, &global, &type_params, NULL);
    qp_symmap_free(&free);
    qp_symmap_free(&global);
    qp_symmap_free(&type_params);
    return r;
}

/* Pop the current block from the stack (entries stay owned by the
   symtable). */
static int
symtable_exit_block(struct symtable *st)
{
    Py_ssize_t size;

    st->st_cur = NULL;
    size = st->st_stack.len;
    if (size) {
        st->st_stack.len = --size;
        if (size)
            st->st_cur = st->st_stack.items[size - 1];
    }
    return 1;
}

static int
symtable_enter_existing_block(struct symtable *st, PySTEntryObject* ste, bool add_to_children)
{
    JSRuntime *rt = st->st_ctx->rt;
    if (qp_stelist_append(rt, &st->st_stack, ste) < 0) {
        return symtable_nomem(st);
    }
    PySTEntryObject *prev = st->st_cur;
    /* bpo-37757: For now, disallow *all* assignment expressions in the
     * outermost iterator expression of a comprehension, even those inside
     * a nested comprehension or a lambda expression.
     */
    if (prev) {
        ste->ste_comp_iter_expr = prev->ste_comp_iter_expr;
    }
    /* No need to inherit ste_mangled_names in classes, where all names
     * are mangled. */
    if (prev && prev->ste_mangled_names != NULL && ste->ste_type != ClassBlock) {
        ste->ste_mangled_names = prev->ste_mangled_names;
    }
    /* The entry is owned by the stack. Borrow it for st_cur. */
    st->st_cur = ste;

    /* If "from __future__ import annotations" is active,
     * annotation blocks shouldn't have any affect on the symbol table since in
     * the compilation stage, they will all be transformed to strings. */
    if (st->st_future->ff_features & CO_FUTURE_ANNOTATIONS && ste->ste_type == AnnotationBlock) {
        return 1;
    }

    if (ste->ste_type == ModuleBlock)
        st->st_global = &st->st_cur->ste_symbols;

    if (add_to_children && prev) {
        if (qp_stelist_append(rt, &prev->ste_children, ste) < 0) {
            symtable_exit_block(st);
            return symtable_nomem(st);
        }
    }
    return 1;
}

static int
symtable_enter_block(struct symtable *st, JSAtom name, _Py_block_ty block,
                     void *ast, _Py_SourceLocation loc)
{
    PySTEntryObject *ste = ste_new(st, name, block, ast, loc);
    if (ste == NULL) {
        return 0;
    }
    int result = symtable_enter_existing_block(st, ste, /* add_to_children */true);
    if (result == 0) {
        return 0;
    }
    if (block == AnnotationBlock || block == TypeVariableBlock || block == TypeAliasBlock) {
        // We need to insert code that reads this "parameter" to the function.
        if (!symtable_add_def(st, st->st_ids.dot_format, DEF_PARAM, loc)) {
            symtable_exit_block(st);
            return 0;
        }
        if (!symtable_add_def(st, st->st_ids.dot_format, USE, loc)) {
            symtable_exit_block(st);
            return 0;
        }
    }
    return 1;
}

static long
symtable_lookup_entry(struct symtable *st, PySTEntryObject *ste, JSAtom name)
{
    JSAtom mangled = symtable_maybe_mangle(st, st->st_private, ste, name);
    if (mangled == JS_ATOM_NULL)
        return -1;
    return _PyST_GetSymbol(ste, mangled);
}

static long
symtable_lookup(struct symtable *st, JSAtom name)
{
    return symtable_lookup_entry(st, st->st_cur, name);
}

static int
symtable_add_def_helper(struct symtable *st, JSAtom name, int flag, struct _symtable_entry *ste,
                        _Py_SourceLocation loc)
{
    JSRuntime *rt = st->st_ctx->rt;
    qp_symmap *dict;
    long val;
    JSAtom mangled = symtable_maybe_mangle(st, st->st_private, st->st_cur, name);

    if (mangled == JS_ATOM_NULL)
        return 0;
    dict = &ste->ste_symbols;
    if (qp_symmap_lookup(dict, mangled, &val)) {
        if ((flag & DEF_PARAM) && (val & DEF_PARAM)) {
            /* Is it better to use 'mangled' or 'name' here? */
            return symtable_error_name(st, loc, DUPLICATE_PARAMETER, name);
        }
        if ((flag & DEF_TYPE_PARAM) && (val & DEF_TYPE_PARAM)) {
            return symtable_error_name(st, loc, DUPLICATE_TYPE_PARAM, name);
        }
        val |= flag;
    }
    else {
        val = flag;
    }
    if (ste->ste_comp_iter_target) {
        /* This name is an iteration variable in a comprehension,
         * so check for a binding conflict with any named expressions.
         * Otherwise, mark it as an iteration variable so subsequent
         * named expressions can check for conflicts.
         */
        if (val & (DEF_GLOBAL | DEF_NONLOCAL)) {
            return symtable_error_name(st, loc, NAMED_EXPR_COMP_INNER_LOOP_CONFLICT, name);
        }
        val |= DEF_COMP_ITER;
    }
    if (qp_symmap_set(dict, mangled, val) < 0)
        return symtable_nomem(st);

    if (flag & DEF_PARAM) {
        if (qp_atomlist_append(rt, &ste->ste_varnames, mangled) < 0)
            return symtable_nomem(st);
    } else if (flag & DEF_GLOBAL) {
        /* XXX need to update DEF_GLOBAL for other flags too;
           perhaps only DEF_FREE_GLOBAL */
        val = 0;
        qp_symmap_lookup(st->st_global, mangled, &val);
        val |= flag;
        if (qp_symmap_set(st->st_global, mangled, val) < 0)
            return symtable_nomem(st);
    }
    return 1;
}

static int
check_name(struct symtable *st, JSAtom name, _Py_SourceLocation loc,
           expr_context_ty ctx)
{
    if (ctx == Store && qp_st_atom_eq_ascii(st->st_ctx, name, "__debug__")) {
        return symtable_error(st, loc, "%s", "cannot assign to __debug__");
    }
    if (ctx == Del && qp_st_atom_eq_ascii(st->st_ctx, name, "__debug__")) {
        return symtable_error(st, loc, "%s", "cannot delete __debug__");
    }
    return 1;
}

static int
check_keywords(struct symtable *st, asdl_keyword_seq *keywords)
{
    for (Py_ssize_t i = 0; i < asdl_seq_LEN(keywords); i++) {
        keyword_ty key = ((keyword_ty)asdl_seq_GET(keywords, i));
        if (key->arg != JS_ATOM_NULL && !check_name(st, key->arg, LOCATION(key), Store)) {
            return 0;
        }
    }
    return 1;
}

static int
check_kwd_patterns(struct symtable *st, pattern_ty p)
{
    assert(p->kind == MatchClass_kind);
    asdl_identifier_seq *kwd_attrs = p->v.MatchClass.kwd_attrs;
    asdl_pattern_seq *kwd_patterns = p->v.MatchClass.kwd_patterns;
    for (Py_ssize_t i = 0; i < asdl_seq_LEN(kwd_attrs); i++) {
        _Py_SourceLocation loc = LOCATION(asdl_seq_GET(kwd_patterns, i));
        if (!check_name(st, asdl_seq_GET(kwd_attrs, i), loc, Store)) {
            return 0;
        }
    }
    return 1;
}

static int
symtable_add_def_ctx(struct symtable *st, JSAtom name, int flag,
                     _Py_SourceLocation loc, expr_context_ty ctx)
{
    int write_mask = DEF_PARAM | DEF_LOCAL | DEF_IMPORT;
    if ((flag & write_mask) && !check_name(st, name, loc, ctx)) {
        return 0;
    }
    if ((flag & DEF_TYPE_PARAM) && st->st_cur->ste_mangled_names != NULL) {
        if (qp_symmap_set(st->st_cur->ste_mangled_names, name, 0) < 0) {
            return symtable_nomem(st);
        }
    }
    return symtable_add_def_helper(st, name, flag, st->st_cur, loc);
}

static int
symtable_add_def(struct symtable *st, JSAtom name, int flag,
                 _Py_SourceLocation loc)
{
    return symtable_add_def_ctx(st, name, flag, loc,
                                flag == USE ? Load : Store);
}

static int
symtable_enter_type_param_block(struct symtable *st, JSAtom name,
                               void *ast, int has_defaults, int has_kwdefaults,
                               enum _stmt_kind kind, _Py_SourceLocation loc)
{
    _Py_block_ty current_type = st->st_cur->ste_type;
    if(!symtable_enter_block(st, name, TypeParametersBlock, ast, loc)) {
        return 0;
    }
    if (current_type == ClassBlock) {
        st->st_cur->ste_can_see_class_scope = 1;
        if (!symtable_add_def(st, st->st_ids.dunder_classdict, USE, loc)) {
            goto error;
        }
    }
    if (kind == ClassDef_kind) {
        // It gets "set" when we create the type params tuple and
        // "used" when we build up the bases.
        if (!symtable_add_def(st, st->st_ids.dot_type_params, DEF_LOCAL, loc)) {
            goto error;
        }
        if (!symtable_add_def(st, st->st_ids.dot_type_params, USE, loc)) {
            goto error;
        }
        // This is used for setting the generic base
        if (!symtable_add_def(st, st->st_ids.dot_generic_base, DEF_LOCAL, loc)) {
            goto error;
        }
        if (!symtable_add_def(st, st->st_ids.dot_generic_base, USE, loc)) {
            goto error;
        }
    }
    if (has_defaults) {
        if (!symtable_add_def(st, st->st_ids.dot_defaults, DEF_PARAM, loc)) {
            goto error;
        }
    }
    if (has_kwdefaults) {
        if (!symtable_add_def(st, st->st_ids.dot_kwdefaults, DEF_PARAM, loc)) {
            goto error;
        }
    }
    return 1;
error:
    symtable_exit_block(st);
    return 0;
}

/* VISIT, VISIT_SEQ and VIST_SEQ_TAIL take an ASDL type as their second argument.
   They use the ASDL name to synthesize the name of the C type and the visit
   function.

   VISIT_SEQ_TAIL permits the start of an ASDL sequence to be skipped, which is
   useful if the first node in the sequence requires special treatment.

   ENTER_RECURSIVE macro checks the remaining C stack.
   It should be used at the beginning of the recursive function.

   LEAVE_RECURSIVE macro marks the end of the recursive function (no-op:
   the stack check needs no counter).
*/

#define VISIT(ST, TYPE, V) \
    do { \
        if (!symtable_visit_ ## TYPE((ST), (V))) { \
            return 0; \
        } \
    } while(0)

#define VISIT_SEQ(ST, TYPE, SEQ) \
    do { \
        Py_ssize_t i; \
        asdl_ ## TYPE ## _seq *seq = (SEQ); /* avoid variable capture */ \
        for (i = 0; i < asdl_seq_LEN(seq); i++) { \
            TYPE ## _ty elt = (TYPE ## _ty)asdl_seq_GET(seq, i); \
            if (!symtable_visit_ ## TYPE((ST), elt)) \
                return 0;                 \
        } \
    } while(0)

#define VISIT_SEQ_TAIL(ST, TYPE, SEQ, START) \
    do { \
        Py_ssize_t i; \
        asdl_ ## TYPE ## _seq *seq = (SEQ); /* avoid variable capture */ \
        for (i = (START); i < asdl_seq_LEN(seq); i++) { \
            TYPE ## _ty elt = (TYPE ## _ty)asdl_seq_GET(seq, i); \
            if (!symtable_visit_ ## TYPE((ST), elt)) \
                return 0;                 \
        } \
    } while(0)

#define VISIT_SEQ_WITH_NULL(ST, TYPE, SEQ) \
    do { \
        int i = 0; \
        asdl_ ## TYPE ## _seq *seq = (SEQ); /* avoid variable capture */ \
        for (i = 0; i < asdl_seq_LEN(seq); i++) { \
            TYPE ## _ty elt = (TYPE ## _ty)asdl_seq_GET(seq, i); \
            if (!elt) continue; /* can be NULL */ \
            if (!symtable_visit_ ## TYPE((ST), elt)) \
                return 0;             \
        } \
    } while(0)

#define ENTER_CONDITIONAL_BLOCK(ST) \
    int in_conditional_block = (ST)->st_cur->ste_in_conditional_block; \
    (ST)->st_cur->ste_in_conditional_block = 1;

#define LEAVE_CONDITIONAL_BLOCK(ST) \
    (ST)->st_cur->ste_in_conditional_block = in_conditional_block;

#define ENTER_TRY_BLOCK(ST) \
    int in_try_block = (ST)->st_cur->ste_in_try_block; \
    (ST)->st_cur->ste_in_try_block = 1;

#define LEAVE_TRY_BLOCK(ST) \
    (ST)->st_cur->ste_in_try_block = in_try_block;

/* Py_EnterRecursiveCall(" during compilation") */
static int
symtable_enter_recursive(struct symtable *st)
{
    if (js_check_stack_overflow(st->st_ctx->rt, 0)) {
        qp_error_set(st->st_err, QP_ERR_RECURSION, 0, 0, 0, 0, "%s",
                     "maximum recursion depth exceeded during compilation");
        return -1;
    }
    return 0;
}

#define ENTER_RECURSIVE() \
if (symtable_enter_recursive(st)) { \
    return 0; \
}

#define LEAVE_RECURSIVE()


static int
symtable_record_directive(struct symtable *st, JSAtom name, _Py_SourceLocation loc)
{
    JSAtom mangled = symtable_maybe_mangle(st, st->st_private, st->st_cur, name);
    if (mangled == JS_ATOM_NULL)
        return 0;
    if (qp_directives_append(st->st_ctx->rt, &st->st_cur->ste_directives, mangled, loc) < 0)
        return symtable_nomem(st);
    return 1;
}

static int
has_kwonlydefaults(asdl_arg_seq *kwonlyargs, asdl_expr_seq *kw_defaults)
{
    for (int i = 0; i < asdl_seq_LEN(kwonlyargs); i++) {
        expr_ty default_ = asdl_seq_GET(kw_defaults, i);
        if (default_) {
            return 1;
        }
    }
    return 0;
}

static int
check_import_from(struct symtable *st, stmt_ty s)
{
    assert(s->kind == ImportFrom_kind);
    _Py_SourceLocation fut = st->st_future->ff_location;
    if (s->v.ImportFrom.module != JS_ATOM_NULL && s->v.ImportFrom.level == 0 &&
        qp_st_atom_eq_ascii(st->st_ctx, s->v.ImportFrom.module, "__future__") &&
        ((s->lineno > fut.lineno) ||
         ((s->lineno == fut.end_lineno) && (s->col_offset > fut.end_col_offset))))
    {
        return symtable_error(st, LOCATION(s), "%s",
                              "from __future__ imports must occur "
                              "at the beginning of the file");
    }
    return 1;
}

static int
check_lazy_import_context(struct symtable *st, stmt_ty s,
                          const char* import_type)
{
    // Check if inside try/except block.
    if (st->st_cur->ste_in_try_block) {
        return symtable_error(st, LOCATION(s),
                              "lazy %s not allowed inside try/except blocks",
                              import_type);
    }

    // Check if inside function scope.
    if (st->st_cur->ste_type == FunctionBlock) {
        return symtable_error(st, LOCATION(s),
                              "lazy %s not allowed inside functions", import_type);
    }

    // Check if inside class scope.
    if (st->st_cur->ste_type == ClassBlock) {
        return symtable_error(st, LOCATION(s),
                              "lazy %s not allowed inside classes", import_type);
    }

    return 1;
}

static bool
allows_top_level_await(struct symtable *st)
{
    return (st->st_future->ff_features & QP_PF_TOP_LEVEL_AWAIT) &&
            st->st_cur->ste_type == ModuleBlock;
}


static void
maybe_set_ste_coroutine_for_module(struct symtable *st, stmt_ty s)
{
    if (allows_top_level_await(st)) {
        st->st_cur->ste_coroutine = 1;
    }
}

static int
symtable_visit_stmt(struct symtable *st, stmt_ty s)
{
    ENTER_RECURSIVE();
    switch (s->kind) {
    case FunctionDef_kind: {
        if (!symtable_add_def(st, s->v.FunctionDef.name, DEF_LOCAL, LOCATION(s)))
            return 0;
        if (s->v.FunctionDef.args->defaults)
            VISIT_SEQ(st, expr, s->v.FunctionDef.args->defaults);
        if (s->v.FunctionDef.args->kw_defaults)
            VISIT_SEQ_WITH_NULL(st, expr, s->v.FunctionDef.args->kw_defaults);
        if (s->v.FunctionDef.decorator_list)
            VISIT_SEQ(st, expr, s->v.FunctionDef.decorator_list);
        if (asdl_seq_LEN(s->v.FunctionDef.type_params) > 0) {
            if (!symtable_enter_type_param_block(
                    st, s->v.FunctionDef.name,
                    (void *)s->v.FunctionDef.type_params,
                    s->v.FunctionDef.args->defaults != NULL,
                    has_kwonlydefaults(s->v.FunctionDef.args->kwonlyargs,
                                       s->v.FunctionDef.args->kw_defaults),
                    s->kind,
                    LOCATION(s))) {
                return 0;
            }
            VISIT_SEQ(st, type_param, s->v.FunctionDef.type_params);
        }
        PySTEntryObject *new_ste = ste_new(st, s->v.FunctionDef.name, FunctionBlock, (void *)s,
                                           LOCATION(s));
        if (!new_ste) {
            return 0;
        }

        if (_PyAST_GetDocString(s->v.FunctionDef.body)) {
            new_ste->ste_has_docstring = 1;
        }

        if (!symtable_visit_annotations(st, s, s->v.FunctionDef.args,
                                        s->v.FunctionDef.returns, new_ste)) {
            return 0;
        }
        if (!symtable_enter_existing_block(st, new_ste, /* add_to_children */true)) {
            return 0;
        }
        VISIT(st, arguments, s->v.FunctionDef.args);
        VISIT_SEQ(st, stmt, s->v.FunctionDef.body);
        if (!symtable_exit_block(st))
            return 0;
        if (asdl_seq_LEN(s->v.FunctionDef.type_params) > 0) {
            if (!symtable_exit_block(st))
                return 0;
        }
        break;
    }
    case ClassDef_kind: {
        JSAtom tmp;
        if (!symtable_add_def(st, s->v.ClassDef.name, DEF_LOCAL, LOCATION(s)))
            return 0;
        if (s->v.ClassDef.decorator_list)
            VISIT_SEQ(st, expr, s->v.ClassDef.decorator_list);
        tmp = st->st_private;
        if (asdl_seq_LEN(s->v.ClassDef.type_params) > 0) {
            if (!symtable_enter_type_param_block(st, s->v.ClassDef.name,
                                                (void *)s->v.ClassDef.type_params,
                                                false, false, s->kind,
                                                LOCATION(s))) {
                return 0;
            }
            st->st_private = s->v.ClassDef.name;
            st->st_cur->ste_mangled_names = symtable_new_set(st);
            if (!st->st_cur->ste_mangled_names) {
                return 0;
            }
            VISIT_SEQ(st, type_param, s->v.ClassDef.type_params);
        }
        VISIT_SEQ(st, expr, s->v.ClassDef.bases);
        if (!check_keywords(st, s->v.ClassDef.keywords)) {
            return 0;
        }
        VISIT_SEQ(st, keyword, s->v.ClassDef.keywords);
        if (!symtable_enter_block(st, s->v.ClassDef.name, ClassBlock,
                                  (void *)s, LOCATION(s))) {
            return 0;
        }
        st->st_private = s->v.ClassDef.name;
        if (asdl_seq_LEN(s->v.ClassDef.type_params) > 0) {
            if (!symtable_add_def(st, st->st_ids.dunder_type_params,
                                  DEF_LOCAL, LOCATION(s))) {
                return 0;
            }
            if (!symtable_add_def(st, st->st_ids.dot_type_params,
                                  USE, LOCATION(s))) {
                return 0;
            }
        }

        if (_PyAST_GetDocString(s->v.ClassDef.body)) {
            st->st_cur->ste_has_docstring = 1;
        }

        VISIT_SEQ(st, stmt, s->v.ClassDef.body);
        if (!symtable_exit_block(st))
            return 0;
        if (asdl_seq_LEN(s->v.ClassDef.type_params) > 0) {
            if (!symtable_exit_block(st))
                return 0;
        }
        st->st_private = tmp;
        break;
    }
    case TypeAlias_kind: {
        VISIT(st, expr, s->v.TypeAlias.name);
        assert(s->v.TypeAlias.name->kind == Name_kind);
        JSAtom name = s->v.TypeAlias.name->v.Name.id;
        int is_in_class = st->st_cur->ste_type == ClassBlock;
        int is_generic = asdl_seq_LEN(s->v.TypeAlias.type_params) > 0;
        if (is_generic) {
            if (!symtable_enter_type_param_block(
                    st, name,
                    (void *)s->v.TypeAlias.type_params,
                    false, false, s->kind,
                    LOCATION(s))) {
                return 0;
            }
            VISIT_SEQ(st, type_param, s->v.TypeAlias.type_params);
        }
        if (!symtable_enter_block(st, name, TypeAliasBlock,
                                  (void *)s, LOCATION(s))) {
            return 0;
        }
        st->st_cur->ste_can_see_class_scope = is_in_class;
        if (is_in_class && !symtable_add_def(st, st->st_ids.dunder_classdict, USE, LOCATION(s->v.TypeAlias.value))) {
            return 0;
        }
        VISIT(st, expr, s->v.TypeAlias.value);
        if (!symtable_exit_block(st))
            return 0;
        if (is_generic) {
            if (!symtable_exit_block(st))
                return 0;
        }
        break;
    }
    case Return_kind:
        if (s->v.Return.value) {
            VISIT(st, expr, s->v.Return.value);
            st->st_cur->ste_returns_value = 1;
        }
        break;
    case Delete_kind:
        VISIT_SEQ(st, expr, s->v.Delete.targets);
        break;
    case Assign_kind:
        VISIT_SEQ(st, expr, s->v.Assign.targets);
        VISIT(st, expr, s->v.Assign.value);
        break;
    case AnnAssign_kind:
        st->st_cur->ste_annotations_used = 1;
        if (s->v.AnnAssign.target->kind == Name_kind) {
            expr_ty e_name = s->v.AnnAssign.target;
            long cur = symtable_lookup(st, e_name->v.Name.id);
            if (cur < 0) {
                return 0;
            }
            if ((cur & (DEF_GLOBAL | DEF_NONLOCAL))
                && (&st->st_cur->ste_symbols != st->st_global)
                && s->v.AnnAssign.simple) {
                return symtable_error_name(st, LOCATION(s),
                                           cur & DEF_GLOBAL ? GLOBAL_ANNOT : NONLOCAL_ANNOT,
                                           e_name->v.Name.id);
            }
            if (s->v.AnnAssign.simple &&
                !symtable_add_def(st, e_name->v.Name.id,
                                  DEF_ANNOT | DEF_LOCAL, LOCATION(e_name))) {
                return 0;
            }
            else {
                if (s->v.AnnAssign.value
                    && !symtable_add_def(st, e_name->v.Name.id, DEF_LOCAL, LOCATION(e_name))) {
                    return 0;
                }
            }
        }
        else {
            VISIT(st, expr, s->v.AnnAssign.target);
        }
        if (!symtable_visit_annotation(st, s->v.AnnAssign.annotation,
                                       (void *)((uintptr_t)st->st_cur->ste_id + 1))) {
            return 0;
        }

        if (s->v.AnnAssign.value) {
            VISIT(st, expr, s->v.AnnAssign.value);
        }
        break;
    case AugAssign_kind: {
        VISIT(st, expr, s->v.AugAssign.target);
        VISIT(st, expr, s->v.AugAssign.value);
        break;
    }
    case For_kind: {
        VISIT(st, expr, s->v.For.target);
        VISIT(st, expr, s->v.For.iter);
        ENTER_CONDITIONAL_BLOCK(st);
        VISIT_SEQ(st, stmt, s->v.For.body);
        if (s->v.For.orelse)
            VISIT_SEQ(st, stmt, s->v.For.orelse);
        LEAVE_CONDITIONAL_BLOCK(st);
        break;
    }
    case While_kind: {
        VISIT(st, expr, s->v.While.test);
        ENTER_CONDITIONAL_BLOCK(st);
        VISIT_SEQ(st, stmt, s->v.While.body);
        if (s->v.While.orelse)
            VISIT_SEQ(st, stmt, s->v.While.orelse);
        LEAVE_CONDITIONAL_BLOCK(st);
        break;
    }
    case If_kind: {
        /* XXX if 0: and lookup_yield() hacks */
        VISIT(st, expr, s->v.If.test);
        ENTER_CONDITIONAL_BLOCK(st);
        VISIT_SEQ(st, stmt, s->v.If.body);
        if (s->v.If.orelse)
            VISIT_SEQ(st, stmt, s->v.If.orelse);
        LEAVE_CONDITIONAL_BLOCK(st);
        break;
    }
    case Match_kind: {
        VISIT(st, expr, s->v.Match.subject);
        ENTER_CONDITIONAL_BLOCK(st);
        VISIT_SEQ(st, match_case, s->v.Match.cases);
        LEAVE_CONDITIONAL_BLOCK(st);
        break;
    }
    case Raise_kind:
        if (s->v.Raise.exc) {
            VISIT(st, expr, s->v.Raise.exc);
            if (s->v.Raise.cause) {
                VISIT(st, expr, s->v.Raise.cause);
            }
        }
        break;
    case Try_kind: {
        ENTER_CONDITIONAL_BLOCK(st);
        ENTER_TRY_BLOCK(st);
        VISIT_SEQ(st, stmt, s->v.Try.body);
        VISIT_SEQ(st, excepthandler, s->v.Try.handlers);
        VISIT_SEQ(st, stmt, s->v.Try.orelse);
        VISIT_SEQ(st, stmt, s->v.Try.finalbody);
        LEAVE_TRY_BLOCK(st);
        LEAVE_CONDITIONAL_BLOCK(st);
        break;
    }
    case TryStar_kind: {
        ENTER_CONDITIONAL_BLOCK(st);
        ENTER_TRY_BLOCK(st);
        VISIT_SEQ(st, stmt, s->v.TryStar.body);
        VISIT_SEQ(st, excepthandler, s->v.TryStar.handlers);
        VISIT_SEQ(st, stmt, s->v.TryStar.orelse);
        VISIT_SEQ(st, stmt, s->v.TryStar.finalbody);
        LEAVE_TRY_BLOCK(st);
        LEAVE_CONDITIONAL_BLOCK(st);
        break;
    }
    case Assert_kind:
        VISIT(st, expr, s->v.Assert.test);
        if (s->v.Assert.msg)
            VISIT(st, expr, s->v.Assert.msg);
        break;
    case Import_kind:
        if (s->v.Import.is_lazy) {
            if (!check_lazy_import_context(st, s, "import")) {
                return 0;
            }
        }
        VISIT_SEQ(st, alias, s->v.Import.names);
        break;
    case ImportFrom_kind:
        if (s->v.ImportFrom.is_lazy) {
            if (!check_lazy_import_context(st, s, "from ... import")) {
                return 0;
            }

            // Check for import *
            for (Py_ssize_t i = 0; i < asdl_seq_LEN(s->v.ImportFrom.names);
                 i++) {
                alias_ty alias = (alias_ty)asdl_seq_GET(
                    s->v.ImportFrom.names, i);
                if (alias->name != JS_ATOM_NULL &&
                        qp_st_atom_eq_ascii(st->st_ctx, alias->name, "*")) {
                    return symtable_error(st, LOCATION(s), "%s",
                                          "lazy from ... import * is not allowed");
                }
            }
        }
        VISIT_SEQ(st, alias, s->v.ImportFrom.names);
        if (!check_import_from(st, s)) {
            return 0;
        }
        break;
    case Global_kind: {
        Py_ssize_t i;
        asdl_identifier_seq *seq = s->v.Global.names;
        for (i = 0; i < asdl_seq_LEN(seq); i++) {
            JSAtom name = asdl_seq_GET(seq, i);
            long cur = symtable_lookup(st, name);
            if (cur < 0)
                return 0;
            if (cur & (DEF_PARAM | DEF_LOCAL | USE | DEF_ANNOT)) {
                const char* msg;
                if (cur & DEF_PARAM) {
                    msg = GLOBAL_PARAM;
                } else if (cur & USE) {
                    msg = GLOBAL_AFTER_USE;
                } else if (cur & DEF_ANNOT) {
                    msg = GLOBAL_ANNOT;
                } else {  /* DEF_LOCAL */
                    msg = GLOBAL_AFTER_ASSIGN;
                }
                return symtable_error_name(st, LOCATION(s), msg, name);
            }
            if (!symtable_add_def(st, name, DEF_GLOBAL, LOCATION(s))) {
                return 0;
            }
            if (!symtable_record_directive(st, name, LOCATION(s))) {
                return 0;
            }
        }
        break;
    }
    case Nonlocal_kind: {
        Py_ssize_t i;
        asdl_identifier_seq *seq = s->v.Nonlocal.names;
        for (i = 0; i < asdl_seq_LEN(seq); i++) {
            JSAtom name = asdl_seq_GET(seq, i);
            long cur = symtable_lookup(st, name);
            if (cur < 0)
                return 0;
            if (cur & (DEF_PARAM | DEF_LOCAL | USE | DEF_ANNOT)) {
                const char* msg;
                if (cur & DEF_PARAM) {
                    msg = NONLOCAL_PARAM;
                } else if (cur & USE) {
                    msg = NONLOCAL_AFTER_USE;
                } else if (cur & DEF_ANNOT) {
                    msg = NONLOCAL_ANNOT;
                } else {  /* DEF_LOCAL */
                    msg = NONLOCAL_AFTER_ASSIGN;
                }
                return symtable_error_name(st, LOCATION(s), msg, name);
            }
            if (!symtable_add_def(st, name, DEF_NONLOCAL, LOCATION(s)))
                return 0;
            if (!symtable_record_directive(st, name, LOCATION(s))) {
                return 0;
            }
        }
        break;
    }
    case Expr_kind:
        VISIT(st, expr, s->v.Expr.value);
        break;
    case Pass_kind:
    case Break_kind:
    case Continue_kind:
        /* nothing to do here */
        break;
    case With_kind: {
        ENTER_CONDITIONAL_BLOCK(st);
        VISIT_SEQ(st, withitem, s->v.With.items);
        VISIT_SEQ(st, stmt, s->v.With.body);
        LEAVE_CONDITIONAL_BLOCK(st);
        break;
    }
    case AsyncFunctionDef_kind: {
        if (!symtable_add_def(st, s->v.AsyncFunctionDef.name, DEF_LOCAL, LOCATION(s)))
            return 0;
        if (s->v.AsyncFunctionDef.args->defaults)
            VISIT_SEQ(st, expr, s->v.AsyncFunctionDef.args->defaults);
        if (s->v.AsyncFunctionDef.args->kw_defaults)
            VISIT_SEQ_WITH_NULL(st, expr,
                                s->v.AsyncFunctionDef.args->kw_defaults);
        if (s->v.AsyncFunctionDef.decorator_list)
            VISIT_SEQ(st, expr, s->v.AsyncFunctionDef.decorator_list);
        if (asdl_seq_LEN(s->v.AsyncFunctionDef.type_params) > 0) {
            if (!symtable_enter_type_param_block(
                    st, s->v.AsyncFunctionDef.name,
                    (void *)s->v.AsyncFunctionDef.type_params,
                    s->v.AsyncFunctionDef.args->defaults != NULL,
                    has_kwonlydefaults(s->v.AsyncFunctionDef.args->kwonlyargs,
                                       s->v.AsyncFunctionDef.args->kw_defaults),
                    s->kind,
                    LOCATION(s))) {
                return 0;
            }
            VISIT_SEQ(st, type_param, s->v.AsyncFunctionDef.type_params);
        }
        PySTEntryObject *new_ste = ste_new(st, s->v.AsyncFunctionDef.name, FunctionBlock, (void *)s,
                                           LOCATION(s));
        if (!new_ste) {
            return 0;
        }

        if (_PyAST_GetDocString(s->v.AsyncFunctionDef.body)) {
            new_ste->ste_has_docstring = 1;
        }

        if (!symtable_visit_annotations(st, s, s->v.AsyncFunctionDef.args,
                                        s->v.AsyncFunctionDef.returns, new_ste)) {
            return 0;
        }
        if (!symtable_enter_existing_block(st, new_ste, /* add_to_children */true)) {
            return 0;
        }

        st->st_cur->ste_coroutine = 1;
        VISIT(st, arguments, s->v.AsyncFunctionDef.args);
        VISIT_SEQ(st, stmt, s->v.AsyncFunctionDef.body);
        if (!symtable_exit_block(st))
            return 0;
        if (asdl_seq_LEN(s->v.AsyncFunctionDef.type_params) > 0) {
            if (!symtable_exit_block(st))
                return 0;
        }
        break;
    }
    case AsyncWith_kind: {
        maybe_set_ste_coroutine_for_module(st, s);
        if (!symtable_raise_if_not_coroutine(st, ASYNC_WITH_OUTSIDE_ASYNC_FUNC, LOCATION(s))) {
            return 0;
        }
        ENTER_CONDITIONAL_BLOCK(st);
        VISIT_SEQ(st, withitem, s->v.AsyncWith.items);
        VISIT_SEQ(st, stmt, s->v.AsyncWith.body);
        LEAVE_CONDITIONAL_BLOCK(st);
        break;
    }
    case AsyncFor_kind: {
        maybe_set_ste_coroutine_for_module(st, s);
        if (!symtable_raise_if_not_coroutine(st, ASYNC_FOR_OUTSIDE_ASYNC_FUNC, LOCATION(s))) {
            return 0;
        }
        VISIT(st, expr, s->v.AsyncFor.target);
        VISIT(st, expr, s->v.AsyncFor.iter);
        ENTER_CONDITIONAL_BLOCK(st);
        VISIT_SEQ(st, stmt, s->v.AsyncFor.body);
        if (s->v.AsyncFor.orelse)
            VISIT_SEQ(st, stmt, s->v.AsyncFor.orelse);
        LEAVE_CONDITIONAL_BLOCK(st);
        break;
    }
    }
    LEAVE_RECURSIVE();
    return 1;
}

static int
symtable_extend_namedexpr_scope(struct symtable *st, expr_ty e)
{
    assert(st->st_stack.len);
    assert(e->kind == Name_kind);

    JSAtom target_name = e->v.Name.id;
    Py_ssize_t i, size;
    struct _symtable_entry *ste;
    size = st->st_stack.len;
    assert(size);

    /* Iterate over the stack in reverse and add to the nearest adequate scope */
    for (i = size - 1; i >= 0; i--) {
        ste = st->st_stack.items[i];

        /* If we find a comprehension scope, check for a target
         * binding conflict with iteration variables, otherwise skip it
         */
        if (ste->ste_comprehension) {
            long target_in_scope = symtable_lookup_entry(st, ste, target_name);
            if (target_in_scope < 0) {
                return 0;
            }
            if ((target_in_scope & DEF_COMP_ITER) &&
                (target_in_scope & DEF_LOCAL)) {
                return symtable_error_name(st, LOCATION(e), NAMED_EXPR_COMP_CONFLICT,
                                           target_name);
            }
            continue;
        }

        /* If we find a FunctionBlock entry, add as GLOBAL/LOCAL or NONLOCAL/LOCAL */
        if (ste->ste_type == FunctionBlock) {
            long target_in_scope = symtable_lookup_entry(st, ste, target_name);
            if (target_in_scope < 0) {
                return 0;
            }
            if (target_in_scope & DEF_GLOBAL) {
                if (!symtable_add_def(st, target_name, DEF_GLOBAL, LOCATION(e)))
                    return 0;
            } else {
                if (!symtable_add_def(st, target_name, DEF_NONLOCAL, LOCATION(e))) {
                    return 0;
                }
            }
            if (!symtable_record_directive(st, target_name, LOCATION(e))) {
                return 0;
            }

            return symtable_add_def_helper(st, target_name, DEF_LOCAL, ste, LOCATION(e));
        }
        /* If we find a ModuleBlock entry, add as GLOBAL */
        if (ste->ste_type == ModuleBlock) {
            if (!symtable_add_def(st, target_name, DEF_GLOBAL, LOCATION(e))) {
                return 0;
            }
            if (!symtable_record_directive(st, target_name, LOCATION(e))) {
                return 0;
            }

            return symtable_add_def_helper(st, target_name, DEF_GLOBAL, ste, LOCATION(e));
        }
        /* Disallow usage in ClassBlock and type scopes */
        if (ste->ste_type == ClassBlock ||
            ste->ste_type == TypeParametersBlock ||
            ste->ste_type == TypeAliasBlock ||
            ste->ste_type == TypeVariableBlock) {
            const char *msg;
            switch (ste->ste_type) {
                case ClassBlock:
                    msg = NAMED_EXPR_COMP_IN_CLASS;
                    break;
                case TypeParametersBlock:
                    msg = NAMED_EXPR_COMP_IN_TYPEPARAM;
                    break;
                case TypeAliasBlock:
                    msg = NAMED_EXPR_COMP_IN_TYPEALIAS;
                    break;
                case TypeVariableBlock:
                    msg = NAMED_EXPR_COMP_IN_TYPEVAR_BOUND;
                    break;
                default:
                    abort(); /* Py_UNREACHABLE */
            }
            return symtable_error(st, LOCATION(e), "%s", msg);
        }
    }

    /* We should always find either a function-like block, ModuleBlock or ClassBlock
       and should never fall to this case
    */
    abort(); /* Py_UNREACHABLE */
    return 0;
}

static int
symtable_handle_namedexpr(struct symtable *st, expr_ty e)
{
    if (st->st_cur->ste_comp_iter_expr > 0) {
        /* Assignment isn't allowed in a comprehension iterable expression */
        return symtable_error(st, LOCATION(e), "%s", NAMED_EXPR_COMP_ITER_EXPR);
    }
    if (st->st_cur->ste_comprehension) {
        /* Inside a comprehension body, so find the right target scope */
        if (!symtable_extend_namedexpr_scope(st, e->v.NamedExpr.target))
            return 0;
    }
    VISIT(st, expr, e->v.NamedExpr.value);
    VISIT(st, expr, e->v.NamedExpr.target);
    return 1;
}

static int
symtable_visit_expr(struct symtable *st, expr_ty e)
{
    ENTER_RECURSIVE();
    switch (e->kind) {
    case NamedExpr_kind:
        if (!symtable_raise_if_annotation_block(st, "named expression", e)) {
            return 0;
        }
        if(!symtable_handle_namedexpr(st, e))
            return 0;
        break;
    case BoolOp_kind:
        VISIT_SEQ(st, expr, e->v.BoolOp.values);
        break;
    case BinOp_kind:
        VISIT(st, expr, e->v.BinOp.left);
        VISIT(st, expr, e->v.BinOp.right);
        break;
    case UnaryOp_kind:
        VISIT(st, expr, e->v.UnaryOp.operand);
        break;
    case Lambda_kind: {
        if (e->v.Lambda.args->defaults)
            VISIT_SEQ(st, expr, e->v.Lambda.args->defaults);
        if (e->v.Lambda.args->kw_defaults)
            VISIT_SEQ_WITH_NULL(st, expr, e->v.Lambda.args->kw_defaults);
        if (!symtable_enter_block(st, st->st_ids.anon_lambda,
                                  FunctionBlock, (void *)e, LOCATION(e))) {
            return 0;
        }
        VISIT(st, arguments, e->v.Lambda.args);
        VISIT(st, expr, e->v.Lambda.body);
        if (!symtable_exit_block(st))
            return 0;
        break;
    }
    case IfExp_kind:
        VISIT(st, expr, e->v.IfExp.test);
        VISIT(st, expr, e->v.IfExp.body);
        VISIT(st, expr, e->v.IfExp.orelse);
        break;
    case Dict_kind:
        VISIT_SEQ_WITH_NULL(st, expr, e->v.Dict.keys);
        VISIT_SEQ(st, expr, e->v.Dict.values);
        break;
    case Set_kind:
        VISIT_SEQ(st, expr, e->v.Set.elts);
        break;
    case GeneratorExp_kind:
        if (!symtable_visit_genexp(st, e))
            return 0;
        break;
    case ListComp_kind:
        if (!symtable_visit_listcomp(st, e))
            return 0;
        break;
    case SetComp_kind:
        if (!symtable_visit_setcomp(st, e))
            return 0;
        break;
    case DictComp_kind:
        if (!symtable_visit_dictcomp(st, e))
            return 0;
        break;
    case Yield_kind:
        if (!symtable_raise_if_annotation_block(st, "yield expression", e)) {
            return 0;
        }
        if (e->v.Yield.value)
            VISIT(st, expr, e->v.Yield.value);
        st->st_cur->ste_generator = 1;
        if (st->st_cur->ste_comprehension) {
            return symtable_raise_if_comprehension_block(st, e);
        }
        break;
    case YieldFrom_kind:
        if (!symtable_raise_if_annotation_block(st, "yield expression", e)) {
            return 0;
        }
        VISIT(st, expr, e->v.YieldFrom.value);
        st->st_cur->ste_generator = 1;
        if (st->st_cur->ste_comprehension) {
            return symtable_raise_if_comprehension_block(st, e);
        }
        break;
    case Await_kind:
        if (!symtable_raise_if_annotation_block(st, "await expression", e)) {
            return 0;
        }
        if (!allows_top_level_await(st)) {
            if (!_PyST_IsFunctionLike(st->st_cur)) {
                return symtable_error(st, LOCATION(e), "%s",
                                      "'await' outside function");
            }
            if (!IS_ASYNC_DEF(st) && st->st_cur->ste_comprehension == NoComprehension) {
                return symtable_error(st, LOCATION(e), "%s",
                                      "'await' outside async function");
            }
        }
        VISIT(st, expr, e->v.Await.value);
        st->st_cur->ste_coroutine = 1;
        break;
    case Compare_kind:
        VISIT(st, expr, e->v.Compare.left);
        VISIT_SEQ(st, expr, e->v.Compare.comparators);
        break;
    case Call_kind:
        VISIT(st, expr, e->v.Call.func);
        VISIT_SEQ(st, expr, e->v.Call.args);
        if (!check_keywords(st, e->v.Call.keywords)) {
            return 0;
        }
        VISIT_SEQ_WITH_NULL(st, keyword, e->v.Call.keywords);
        break;
    case FormattedValue_kind:
        VISIT(st, expr, e->v.FormattedValue.value);
        if (e->v.FormattedValue.format_spec)
            VISIT(st, expr, e->v.FormattedValue.format_spec);
        break;
    case Interpolation_kind:
        VISIT(st, expr, e->v.Interpolation.value);
        if (e->v.Interpolation.format_spec)
            VISIT(st, expr, e->v.Interpolation.format_spec);
        break;
    case JoinedStr_kind:
        VISIT_SEQ(st, expr, e->v.JoinedStr.values);
        break;
    case TemplateStr_kind:
        VISIT_SEQ(st, expr, e->v.TemplateStr.values);
        break;
    case Constant_kind:
        /* Nothing to do here. */
        break;
    /* The following exprs can be assignment targets. */
    case Attribute_kind:
        if (!check_name(st, e->v.Attribute.attr, LOCATION(e), e->v.Attribute.ctx)) {
            return 0;
        }
        VISIT(st, expr, e->v.Attribute.value);
        break;
    case Subscript_kind:
        VISIT(st, expr, e->v.Subscript.value);
        VISIT(st, expr, e->v.Subscript.slice);
        break;
    case Starred_kind:
        VISIT(st, expr, e->v.Starred.value);
        break;
    case Slice_kind:
        if (e->v.Slice.lower)
            VISIT(st, expr, e->v.Slice.lower);
        if (e->v.Slice.upper)
            VISIT(st, expr, e->v.Slice.upper);
        if (e->v.Slice.step)
            VISIT(st, expr, e->v.Slice.step);
        break;
    case Name_kind:
        if (!st->st_cur->ste_in_unevaluated_annotation) {
            if (!symtable_add_def_ctx(st, e->v.Name.id,
                                    e->v.Name.ctx == Load ? USE : DEF_LOCAL,
                                    LOCATION(e), e->v.Name.ctx)) {
                return 0;
            }
            /* Special-case super: it counts as a use of __class__ */
            if (e->v.Name.ctx == Load &&
                _PyST_IsFunctionLike(st->st_cur) &&
                qp_st_atom_eq_ascii(st->st_ctx, e->v.Name.id, "super")) {
                if (!symtable_add_def(st, st->st_ids.dunder_class, USE, LOCATION(e)))
                    return 0;
            }
        }
        break;
    /* child nodes of List and Tuple will have expr_context set */
    case List_kind:
        VISIT_SEQ(st, expr, e->v.List.elts);
        break;
    case Tuple_kind:
        VISIT_SEQ(st, expr, e->v.Tuple.elts);
        break;
    }
    LEAVE_RECURSIVE();
    return 1;
}

static int
symtable_visit_type_param_bound_or_default(
    struct symtable *st, expr_ty e, JSAtom name,
    type_param_ty tp, const char *ste_scope_info)
{
    if (name == st->st_ids.dunder_classdict) {
        return symtable_error_name(st, LOCATION(tp),
                                   "reserved name '%s' cannot be "
                                   "used for type parameter", name);
    }

    if (e) {
        int is_in_class = st->st_cur->ste_can_see_class_scope;
        if (!symtable_enter_block(st, name, TypeVariableBlock, (void *)tp, LOCATION(e))) {
            return 0;
        }

        st->st_cur->ste_can_see_class_scope = is_in_class;
        if (is_in_class && !symtable_add_def(st, st->st_ids.dunder_classdict, USE, LOCATION(e))) {
            return 0;
        }

        assert(ste_scope_info != NULL);
        st->st_cur->ste_scope_info = ste_scope_info;
        VISIT(st, expr, e);

        if (!symtable_exit_block(st)) {
            return 0;
        }
    }
    return 1;
}

static int
symtable_visit_type_param(struct symtable *st, type_param_ty tp)
{
    ENTER_RECURSIVE();
    switch(tp->kind) {
    case TypeVar_kind: {
        if (!symtable_add_def(st, tp->v.TypeVar.name, DEF_TYPE_PARAM | DEF_LOCAL, LOCATION(tp)))
            return 0;

        const char *ste_scope_info = NULL;
        const expr_ty bound = tp->v.TypeVar.bound;
        if (bound != NULL) {
            ste_scope_info = bound->kind == Tuple_kind ? "a TypeVar constraint" : "a TypeVar bound";
        }

        // We must use a different key for the bound and default. The obvious choice would be to
        // use the .bound and .default_value pointers, but that fails when the expression immediately
        // inside the bound or default is a comprehension: we would reuse the same key for
        // the comprehension scope. Therefore, use the address + 1 as the second key.
        // The only requirement for the key is that it is unique and it matches the logic in
        // compile.c where the scope is retrieved.
        if (!symtable_visit_type_param_bound_or_default(st, tp->v.TypeVar.bound, tp->v.TypeVar.name,
                                                        tp, ste_scope_info)) {
            return 0;
        }

        if (!symtable_visit_type_param_bound_or_default(st, tp->v.TypeVar.default_value, tp->v.TypeVar.name,
                                                        (type_param_ty)((uintptr_t)tp + 1), "a TypeVar default")) {
            return 0;
        }
        break;
    }
    case TypeVarTuple_kind:
        if (!symtable_add_def(st, tp->v.TypeVarTuple.name, DEF_TYPE_PARAM | DEF_LOCAL, LOCATION(tp))) {
            return 0;
        }

        if (!symtable_visit_type_param_bound_or_default(st, tp->v.TypeVarTuple.default_value, tp->v.TypeVarTuple.name,
                                                        tp, "a TypeVarTuple default")) {
            return 0;
        }
        break;
    case ParamSpec_kind:
        if (!symtable_add_def(st, tp->v.ParamSpec.name, DEF_TYPE_PARAM | DEF_LOCAL, LOCATION(tp))) {
            return 0;
        }

        if (!symtable_visit_type_param_bound_or_default(st, tp->v.ParamSpec.default_value, tp->v.ParamSpec.name,
                                                        tp, "a ParamSpec default")) {
            return 0;
        }
        break;
    }
    LEAVE_RECURSIVE();
    return 1;
}

static int
symtable_visit_pattern(struct symtable *st, pattern_ty p)
{
    ENTER_RECURSIVE();
    switch (p->kind) {
    case MatchValue_kind:
        VISIT(st, expr, p->v.MatchValue.value);
        break;
    case MatchSingleton_kind:
        /* Nothing to do here. */
        break;
    case MatchSequence_kind:
        VISIT_SEQ(st, pattern, p->v.MatchSequence.patterns);
        break;
    case MatchStar_kind:
        if (p->v.MatchStar.name != JS_ATOM_NULL) {
            if (!symtable_add_def(st, p->v.MatchStar.name, DEF_LOCAL, LOCATION(p))) {
                return 0;
            }
        }
        break;
    case MatchMapping_kind:
        VISIT_SEQ(st, expr, p->v.MatchMapping.keys);
        VISIT_SEQ(st, pattern, p->v.MatchMapping.patterns);
        if (p->v.MatchMapping.rest != JS_ATOM_NULL) {
            if (!symtable_add_def(st, p->v.MatchMapping.rest, DEF_LOCAL, LOCATION(p))) {
                return 0;
            }
        }
        break;
    case MatchClass_kind:
        VISIT(st, expr, p->v.MatchClass.cls);
        VISIT_SEQ(st, pattern, p->v.MatchClass.patterns);
        if (!check_kwd_patterns(st, p)) {
            return 0;
        }
        VISIT_SEQ(st, pattern, p->v.MatchClass.kwd_patterns);
        break;
    case MatchAs_kind:
        if (p->v.MatchAs.pattern) {
            VISIT(st, pattern, p->v.MatchAs.pattern);
        }
        if (p->v.MatchAs.name != JS_ATOM_NULL) {
            if (!symtable_add_def(st, p->v.MatchAs.name, DEF_LOCAL, LOCATION(p))) {
                return 0;
            }
        }
        break;
    case MatchOr_kind:
        VISIT_SEQ(st, pattern, p->v.MatchOr.patterns);
        break;
    }
    LEAVE_RECURSIVE();
    return 1;
}

static int
symtable_implicit_arg(struct symtable *st, int pos)
{
    char buf[32];
    int n = snprintf(buf, sizeof(buf), ".%d", pos);
    JSAtom id = symtable_new_atom(st, buf, (size_t)n);
    if (id == JS_ATOM_NULL)
        return 0;
    if (!symtable_add_def(st, id, DEF_PARAM, st->st_cur->ste_loc)) {
        return 0;
    }
    return 1;
}

static int
symtable_visit_params(struct symtable *st, asdl_arg_seq *args)
{
    Py_ssize_t i;

    for (i = 0; i < asdl_seq_LEN(args); i++) {
        arg_ty arg = (arg_ty)asdl_seq_GET(args, i);
        if (!symtable_add_def(st, arg->arg, DEF_PARAM, LOCATION(arg)))
            return 0;
    }

    return 1;
}

static int
symtable_visit_annotation(struct symtable *st, expr_ty annotation, void *key)
{
    // Annotations in local scopes are not executed and should not affect the symtable
    bool is_unevaluated = st->st_cur->ste_type == FunctionBlock;

    // Module-level annotations are always considered conditional because the module
    // may be partially executed.
    if ((((st->st_cur->ste_type == ClassBlock && st->st_cur->ste_in_conditional_block)
            || st->st_cur->ste_type == ModuleBlock))
            && !st->st_cur->ste_has_conditional_annotations)
    {
        st->st_cur->ste_has_conditional_annotations = 1;
        if (!symtable_add_def(st, st->st_ids.dunder_conditional_annotations, USE, LOCATION(annotation))) {
            return 0;
        }
    }
    struct _symtable_entry *parent_ste = st->st_cur;
    if (parent_ste->ste_annotation_block == NULL) {
        _Py_block_ty current_type = parent_ste->ste_type;
        if (!symtable_enter_block(st, st->st_ids.dunder_annotate, AnnotationBlock,
                                    key, LOCATION(annotation))) {
            return 0;
        }
        parent_ste->ste_annotation_block = st->st_cur;
        int future_annotations = st->st_future->ff_features & CO_FUTURE_ANNOTATIONS;
        if (current_type == ClassBlock && !future_annotations) {
            st->st_cur->ste_can_see_class_scope = 1;
            parent_ste->ste_needs_classdict = 1;
            if (!symtable_add_def(st, st->st_ids.dunder_classdict, USE, LOCATION(annotation))) {
                return 0;
            }
        }
    }
    else {
        if (!symtable_enter_existing_block(st, parent_ste->ste_annotation_block,
                                           /* add_to_children */false)) {
            return 0;
        }
    }
    if (is_unevaluated) {
        st->st_cur->ste_in_unevaluated_annotation = 1;
    }
    int rc = symtable_visit_expr(st, annotation);
    if (is_unevaluated) {
        st->st_cur->ste_in_unevaluated_annotation = 0;
    }
    if (!symtable_exit_block(st)) {
        return 0;
    }
    return rc;
}

static int
symtable_visit_argannotations(struct symtable *st, asdl_arg_seq *args)
{
    Py_ssize_t i;

    for (i = 0; i < asdl_seq_LEN(args); i++) {
        arg_ty arg = (arg_ty)asdl_seq_GET(args, i);
        if (arg->annotation) {
            st->st_cur->ste_annotations_used = 1;
            VISIT(st, expr, arg->annotation);
        }
    }

    return 1;
}

static int
symtable_visit_annotations(struct symtable *st, stmt_ty o, arguments_ty a, expr_ty returns,
                           struct _symtable_entry *function_ste)
{
    int is_in_class = st->st_cur->ste_can_see_class_scope;
    _Py_block_ty current_type = st->st_cur->ste_type;
    if (!symtable_enter_block(st, st->st_ids.dunder_annotate, AnnotationBlock,
                              (void *)a, LOCATION(o))) {
        return 0;
    }
    st->st_cur->ste_function_name = function_ste->ste_name;
    if (is_in_class || current_type == ClassBlock) {
        st->st_cur->ste_can_see_class_scope = 1;
        if (!symtable_add_def(st, st->st_ids.dunder_classdict, USE, LOCATION(o))) {
            return 0;
        }
    }
    if (a->posonlyargs && !symtable_visit_argannotations(st, a->posonlyargs))
        return 0;
    if (a->args && !symtable_visit_argannotations(st, a->args))
        return 0;
    if (a->vararg && a->vararg->annotation) {
        st->st_cur->ste_annotations_used = 1;
        VISIT(st, expr, a->vararg->annotation);
    }
    if (a->kwarg && a->kwarg->annotation) {
        st->st_cur->ste_annotations_used = 1;
        VISIT(st, expr, a->kwarg->annotation);
    }
    if (a->kwonlyargs && !symtable_visit_argannotations(st, a->kwonlyargs))
        return 0;
    if (returns) {
        st->st_cur->ste_annotations_used = 1;
        VISIT(st, expr, returns);
    }
    if (!symtable_exit_block(st)) {
        return 0;
    }
    return 1;
}

static int
symtable_visit_arguments(struct symtable *st, arguments_ty a)
{
    /* skip default arguments inside function block
       XXX should ast be different?
    */
    if (a->posonlyargs && !symtable_visit_params(st, a->posonlyargs))
        return 0;
    if (a->args && !symtable_visit_params(st, a->args))
        return 0;
    if (a->kwonlyargs && !symtable_visit_params(st, a->kwonlyargs))
        return 0;
    if (a->vararg) {
        if (!symtable_add_def(st, a->vararg->arg, DEF_PARAM, LOCATION(a->vararg)))
            return 0;
        st->st_cur->ste_varargs = 1;
    }
    if (a->kwarg) {
        if (!symtable_add_def(st, a->kwarg->arg, DEF_PARAM, LOCATION(a->kwarg)))
            return 0;
        st->st_cur->ste_varkeywords = 1;
    }
    return 1;
}


static int
symtable_visit_excepthandler(struct symtable *st, excepthandler_ty eh)
{
    if (eh->v.ExceptHandler.type)
        VISIT(st, expr, eh->v.ExceptHandler.type);
    if (eh->v.ExceptHandler.name != JS_ATOM_NULL)
        if (!symtable_add_def(st, eh->v.ExceptHandler.name, DEF_LOCAL, LOCATION(eh)))
            return 0;
    VISIT_SEQ(st, stmt, eh->v.ExceptHandler.body);
    return 1;
}

static int
symtable_visit_withitem(struct symtable *st, withitem_ty item)
{
    VISIT(st, expr, item->context_expr);
    if (item->optional_vars) {
        VISIT(st, expr, item->optional_vars);
    }
    return 1;
}

static int
symtable_visit_match_case(struct symtable *st, match_case_ty m)
{
    VISIT(st, pattern, m->pattern);
    if (m->guard) {
        VISIT(st, expr, m->guard);
    }
    VISIT_SEQ(st, stmt, m->body);
    return 1;
}

static int
symtable_visit_alias(struct symtable *st, alias_ty a)
{
    /* Compute store_name, the name actually bound by the import
       operation.  It is different than a->name when a->name is a
       dotted package name (e.g. spam.eggs)
    */
    JSAtom store_name;
    JSAtom name = (a->asname == JS_ATOM_NULL) ? a->name : a->asname;
    JSString *p = qp_st_atom_str(st->st_ctx, name);
    int64_t dot = p ? qp_st_find_char(p, '.') : -1;
    if (dot != -1) {
        /* store_name = name[:dot] */
        StringBuffer b;
        JSValue v;
        string_buffer_init(st->st_ctx, &b, (int)dot);
        string_buffer_concat(&b, p, 0, (uint32_t)dot);
        v = string_buffer_end(&b);
        if (JS_IsException(v)) {
            qp_st_nomem(st->st_ctx, st->st_err, 1);
            return 0;
        }
        store_name = JS_NewAtomStr(st->st_ctx, JS_VALUE_GET_STRING(v));
        if (store_name == JS_ATOM_NULL) {
            qp_st_nomem(st->st_ctx, st->st_err, 1);
            return 0;
        }
        if (symtable_adopt_atom(st, store_name) < 0)
            return 0;
    }
    else {
        store_name = name;
    }
    if (!qp_st_atom_eq_ascii(st->st_ctx, name, "*")) {
        return symtable_add_def(st, store_name, DEF_IMPORT, LOCATION(a));
    }
    else {
        if (st->st_cur->ste_type != ModuleBlock) {
            return symtable_error(st, LOCATION(a), "%s", IMPORT_STAR_WARNING);
        }
        return 1;
    }
}


static int
symtable_visit_comprehension(struct symtable *st, comprehension_ty lc)
{
    st->st_cur->ste_comp_iter_target = 1;
    VISIT(st, expr, lc->target);
    st->st_cur->ste_comp_iter_target = 0;
    st->st_cur->ste_comp_iter_expr++;
    VISIT(st, expr, lc->iter);
    st->st_cur->ste_comp_iter_expr--;
    VISIT_SEQ(st, expr, lc->ifs);
    if (lc->is_async) {
        st->st_cur->ste_coroutine = 1;
    }
    return 1;
}


static int
symtable_visit_keyword(struct symtable *st, keyword_ty k)
{
    VISIT(st, expr, k->value);
    return 1;
}


static int
symtable_handle_comprehension(struct symtable *st, expr_ty e,
                              JSAtom scope_name, asdl_comprehension_seq *generators,
                              expr_ty elt, expr_ty value)
{
    int is_generator = (e->kind == GeneratorExp_kind);
    comprehension_ty outermost = ((comprehension_ty)
                                    asdl_seq_GET(generators, 0));
    /* Outermost iterator is evaluated in current scope */
    st->st_cur->ste_comp_iter_expr++;
    VISIT(st, expr, outermost->iter);
    st->st_cur->ste_comp_iter_expr--;
    /* Create comprehension scope for the rest */
    if (scope_name == JS_ATOM_NULL ||
        !symtable_enter_block(st, scope_name, FunctionBlock, (void *)e, LOCATION(e))) {
        return 0;
    }
    switch(e->kind) {
        case ListComp_kind:
            st->st_cur->ste_comprehension = ListComprehension;
            break;
        case SetComp_kind:
            st->st_cur->ste_comprehension = SetComprehension;
            break;
        case DictComp_kind:
            st->st_cur->ste_comprehension = DictComprehension;
            break;
        default:
            st->st_cur->ste_comprehension = GeneratorExpression;
            break;
    }
    if (outermost->is_async) {
        st->st_cur->ste_coroutine = 1;
    }

    /* Outermost iter is received as an argument */
    if (!symtable_implicit_arg(st, 0)) {
        symtable_exit_block(st);
        return 0;
    }
    /* Visit iteration variable target, and mark them as such */
    st->st_cur->ste_comp_iter_target = 1;
    VISIT(st, expr, outermost->target);
    st->st_cur->ste_comp_iter_target = 0;
    /* Visit the rest of the comprehension body */
    VISIT_SEQ(st, expr, outermost->ifs);
    VISIT_SEQ_TAIL(st, comprehension, generators, 1);
    if (value)
        VISIT(st, expr, value);
    VISIT(st, expr, elt);
    st->st_cur->ste_generator = is_generator;
    int is_async = st->st_cur->ste_coroutine && !is_generator;
    if (!symtable_exit_block(st)) {
        return 0;
    }
    if (is_async &&
        !IS_ASYNC_DEF(st) &&
        st->st_cur->ste_comprehension == NoComprehension &&
        !allows_top_level_await(st))
    {
        return symtable_error(st, LOCATION(e), "%s",
                              "asynchronous comprehension outside of "
                              "an asynchronous function");
    }
    if (is_async) {
        st->st_cur->ste_coroutine = 1;
    }
    return 1;
}

static int
symtable_visit_genexp(struct symtable *st, expr_ty e)
{
    return symtable_handle_comprehension(st, e, st->st_ids.anon_genexpr,
                                         e->v.GeneratorExp.generators,
                                         e->v.GeneratorExp.elt, NULL);
}

static int
symtable_visit_listcomp(struct symtable *st, expr_ty e)
{
    return symtable_handle_comprehension(st, e, st->st_ids.anon_listcomp,
                                         e->v.ListComp.generators,
                                         e->v.ListComp.elt, NULL);
}

static int
symtable_visit_setcomp(struct symtable *st, expr_ty e)
{
    return symtable_handle_comprehension(st, e, st->st_ids.anon_setcomp,
                                         e->v.SetComp.generators,
                                         e->v.SetComp.elt, NULL);
}

static int
symtable_visit_dictcomp(struct symtable *st, expr_ty e)
{
    return symtable_handle_comprehension(st, e, st->st_ids.anon_dictcomp,
                                         e->v.DictComp.generators,
                                         e->v.DictComp.key,
                                         e->v.DictComp.value);
}

static int
symtable_raise_if_annotation_block(struct symtable *st, const char *name, expr_ty e)
{
    _Py_block_ty type = st->st_cur->ste_type;
    if (type == AnnotationBlock)
        return symtable_error(st, LOCATION(e), ANNOTATION_NOT_ALLOWED, name);
    else if (type == TypeVariableBlock) {
        const char *info = st->st_cur->ste_scope_info;
        assert(info != NULL); // e.g., info == "a ParamSpec default"
        return symtable_error(st, LOCATION(e), EXPR_NOT_ALLOWED_IN_TYPE_VARIABLE, name, info);
    }
    else if (type == TypeAliasBlock) {
        // for now, we do not have any extra information
        assert(st->st_cur->ste_scope_info == NULL);
        return symtable_error(st, LOCATION(e), EXPR_NOT_ALLOWED_IN_TYPE_ALIAS, name);
    }
    else if (type == TypeParametersBlock) {
        // for now, we do not have any extra information
        assert(st->st_cur->ste_scope_info == NULL);
        return symtable_error(st, LOCATION(e), EXPR_NOT_ALLOWED_IN_TYPE_PARAMETERS, name);
    }
    return 1;
}

static int
symtable_raise_if_comprehension_block(struct symtable *st, expr_ty e) {
    _Py_comprehension_ty type = st->st_cur->ste_comprehension;
    return symtable_error(st, LOCATION(e), "%s",
            (type == ListComprehension) ? "'yield' inside list comprehension" :
            (type == SetComprehension) ? "'yield' inside set comprehension" :
            (type == DictComprehension) ? "'yield' inside dict comprehension" :
            "'yield' inside generator expression");
}

static int
symtable_raise_if_not_coroutine(struct symtable *st, const char *msg, _Py_SourceLocation loc) {
    if (!st->st_cur->ste_coroutine) {
        return symtable_error(st, loc, "%s", msg);
    }
    return 1;
}

/* Unit-local macros (unity build: do not leak into later files). */
#undef QP_SYMMAP_EMPTY
#undef QP_SYMMAP_DUMMY
#undef QP_DUMP_SYMTABLE
#undef GLOBAL_PARAM
#undef NONLOCAL_PARAM
#undef GLOBAL_AFTER_ASSIGN
#undef NONLOCAL_AFTER_ASSIGN
#undef GLOBAL_AFTER_USE
#undef NONLOCAL_AFTER_USE
#undef GLOBAL_ANNOT
#undef NONLOCAL_ANNOT
#undef IMPORT_STAR_WARNING
#undef NAMED_EXPR_COMP_IN_CLASS
#undef NAMED_EXPR_COMP_IN_TYPEVAR_BOUND
#undef NAMED_EXPR_COMP_IN_TYPEALIAS
#undef NAMED_EXPR_COMP_IN_TYPEPARAM
#undef NAMED_EXPR_COMP_CONFLICT
#undef NAMED_EXPR_COMP_INNER_LOOP_CONFLICT
#undef NAMED_EXPR_COMP_ITER_EXPR
#undef ANNOTATION_NOT_ALLOWED
#undef EXPR_NOT_ALLOWED_IN_TYPE_VARIABLE
#undef EXPR_NOT_ALLOWED_IN_TYPE_ALIAS
#undef EXPR_NOT_ALLOWED_IN_TYPE_PARAMETERS
#undef DUPLICATE_TYPE_PARAM
#undef ASYNC_WITH_OUTSIDE_ASYNC_FUNC
#undef ASYNC_FOR_OUTSIDE_ASYNC_FUNC
#undef DUPLICATE_PARAMETER
#undef LOCATION
#undef IS_ASYNC_DEF
#undef VISIT
#undef VISIT_SEQ
#undef VISIT_SEQ_TAIL
#undef VISIT_SEQ_WITH_NULL
#undef ENTER_CONDITIONAL_BLOCK
#undef LEAVE_CONDITIONAL_BLOCK
#undef ENTER_TRY_BLOCK
#undef LEAVE_TRY_BLOCK
#undef ENTER_RECURSIVE
#undef LEAVE_RECURSIVE
