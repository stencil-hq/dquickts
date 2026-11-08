/* SQLite synchronous bindings shared by bun:sqlite and node:sqlite. */
#include "qbun.h"
#include <sqlite3.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <errno.h>
#include <limits.h>

static JSClassID db_id, stmt_id;
static pthread_once_t ids_once = PTHREAD_ONCE_INIT;
static void init_ids(void)
{
    JS_NewClassID(&db_id);
    JS_NewClassID(&stmt_id);
}
typedef struct DB {
    sqlite3 *db;
    int safe;
} DB;
typedef struct Stmt {
    sqlite3_stmt *stmt;
    JSValue owner;
    int safe, bare, strict;
} Stmt;
typedef struct Func {
    JSContext *ctx;
    JSValue callback;
    int safe;
} Func;
static const char *error_name(int code)
{
#define E(x)                                                                                       \
    case x:                                                                                        \
        return #x
    switch (code) {
        E(SQLITE_OK);
        E(SQLITE_ERROR);
        E(SQLITE_INTERNAL);
        E(SQLITE_PERM);
        E(SQLITE_ABORT);
        E(SQLITE_BUSY);
        E(SQLITE_LOCKED);
        E(SQLITE_NOMEM);
        E(SQLITE_READONLY);
        E(SQLITE_INTERRUPT);
        E(SQLITE_IOERR);
        E(SQLITE_CORRUPT);
        E(SQLITE_NOTFOUND);
        E(SQLITE_FULL);
        E(SQLITE_CANTOPEN);
        E(SQLITE_PROTOCOL);
        E(SQLITE_EMPTY);
        E(SQLITE_SCHEMA);
        E(SQLITE_TOOBIG);
        E(SQLITE_CONSTRAINT);
        E(SQLITE_MISMATCH);
        E(SQLITE_MISUSE);
        E(SQLITE_NOLFS);
        E(SQLITE_AUTH);
        E(SQLITE_FORMAT);
        E(SQLITE_RANGE);
        E(SQLITE_NOTADB);
        E(SQLITE_NOTICE);
        E(SQLITE_WARNING);
        E(SQLITE_CONSTRAINT_CHECK);
        E(SQLITE_CONSTRAINT_COMMITHOOK);
        E(SQLITE_CONSTRAINT_FOREIGNKEY);
        E(SQLITE_CONSTRAINT_FUNCTION);
        E(SQLITE_CONSTRAINT_NOTNULL);
        E(SQLITE_CONSTRAINT_PRIMARYKEY);
        E(SQLITE_CONSTRAINT_TRIGGER);
        E(SQLITE_CONSTRAINT_UNIQUE);
        E(SQLITE_CONSTRAINT_VTAB);
        E(SQLITE_CONSTRAINT_ROWID);
        E(SQLITE_BUSY_RECOVERY);
        E(SQLITE_BUSY_SNAPSHOT);
        E(SQLITE_READONLY_RECOVERY);
        E(SQLITE_READONLY_CANTLOCK);
        E(SQLITE_READONLY_ROLLBACK);
        E(SQLITE_CORRUPT_VTAB);
        E(SQLITE_LOCKED_SHAREDCACHE);
        E(SQLITE_ABORT_ROLLBACK);
        E(SQLITE_IOERR_READ);
        E(SQLITE_IOERR_SHORT_READ);
        E(SQLITE_IOERR_WRITE);
        E(SQLITE_IOERR_FSYNC);
        E(SQLITE_IOERR_DIR_FSYNC);
        E(SQLITE_IOERR_TRUNCATE);
        E(SQLITE_IOERR_FSTAT);
        E(SQLITE_IOERR_UNLOCK);
        E(SQLITE_IOERR_RDLOCK);
        E(SQLITE_IOERR_DELETE);
        E(SQLITE_IOERR_BLOCKED);
        E(SQLITE_IOERR_NOMEM);
        E(SQLITE_IOERR_ACCESS);
        E(SQLITE_IOERR_CHECKRESERVEDLOCK);
        E(SQLITE_IOERR_LOCK);
        E(SQLITE_IOERR_CLOSE);
        E(SQLITE_IOERR_DIR_CLOSE);
        E(SQLITE_IOERR_SHMOPEN);
        E(SQLITE_IOERR_SHMSIZE);
        E(SQLITE_IOERR_SHMLOCK);
        E(SQLITE_IOERR_SHMMAP);
        E(SQLITE_IOERR_SEEK);
        E(SQLITE_IOERR_DELETE_NOENT);
        E(SQLITE_IOERR_MMAP);
        E(SQLITE_IOERR_GETTEMPPATH);
        E(SQLITE_IOERR_CONVPATH);
        E(SQLITE_IOERR_VNODE);
        E(SQLITE_IOERR_AUTH);
        E(SQLITE_IOERR_BEGIN_ATOMIC);
        E(SQLITE_IOERR_COMMIT_ATOMIC);
        E(SQLITE_IOERR_ROLLBACK_ATOMIC);
        E(SQLITE_IOERR_DATA);
        E(SQLITE_IOERR_CORRUPTFS);
        E(SQLITE_CANTOPEN_NOTEMPDIR);
        E(SQLITE_CANTOPEN_ISDIR);
        E(SQLITE_CANTOPEN_FULLPATH);
        E(SQLITE_CANTOPEN_CONVPATH);
        E(SQLITE_CANTOPEN_DIRTYWAL);
        E(SQLITE_CANTOPEN_SYMLINK);
        E(SQLITE_CORRUPT_SEQUENCE);
        E(SQLITE_CORRUPT_INDEX);
        E(SQLITE_READONLY_DBMOVED);
        E(SQLITE_READONLY_CANTINIT);
        E(SQLITE_READONLY_DIRECTORY);
        E(SQLITE_CONSTRAINT_PINNED);
        E(SQLITE_CONSTRAINT_DATATYPE);
        E(SQLITE_BUSY_TIMEOUT);
        E(SQLITE_LOCKED_VTAB);
        E(SQLITE_AUTH_USER);
        E(SQLITE_ERROR_MISSING_COLLSEQ);
        E(SQLITE_ERROR_RETRY);
        E(SQLITE_ERROR_SNAPSHOT);
        E(SQLITE_NOTICE_RECOVER_WAL);
        E(SQLITE_NOTICE_RECOVER_ROLLBACK);
        E(SQLITE_WARNING_AUTOINDEX);
    default:
        return "SQLITE_ERROR";
    }
#undef E
}
static JSValue fail(JSContext *ctx, sqlite3 *db, int rc)
{
    int code = db ? sqlite3_extended_errcode(db) : rc;
    if (code == SQLITE_OK)
        code = rc;
    JSValue e = JS_NewError(ctx);
    JS_SetPropertyStr(ctx, e, "name", JS_NewString(ctx, "SQLiteError"));
    JS_SetPropertyStr(ctx, e, "message",
                      JS_NewString(ctx, db ? sqlite3_errmsg(db) : sqlite3_errstr(rc)));
    JS_SetPropertyStr(ctx, e, "code", JS_NewString(ctx, error_name(code)));
    JS_SetPropertyStr(ctx, e, "errno", JS_NewInt32(ctx, code));
    JS_SetPropertyStr(ctx, e, "byteOffset", JS_NewInt32(ctx, db ? sqlite3_error_offset(db) : -1));
    return JS_Throw(ctx, e);
}
static DB *getdb(JSContext *ctx, JSValueConst obj)
{
    DB *d = JS_GetOpaque2(ctx, obj, db_id);
    if (d && !d->db) {
        qb_throw_code(ctx, JS_TYPE_ERROR, "ERR_INVALID_STATE", "Database is closed");
        return NULL;
    }
    return d;
}
static Stmt *getstmt(JSContext *ctx, JSValueConst obj)
{
    Stmt *s = JS_GetOpaque2(ctx, obj, stmt_id);
    if (s && (!s->stmt || !getdb(ctx, s->owner))) {
        if (!s->stmt)
            qb_throw_code(ctx, JS_TYPE_ERROR, "ERR_INVALID_STATE", "Statement has been finalized");
        return NULL;
    }
    return s;
}
static void db_free(JSRuntime *rt, JSValue obj)
{
    DB *d = JS_GetOpaque(obj, db_id);
    (void)rt;
    if (d) {
        if (d->db)
            sqlite3_close_v2(d->db);
        free(d);
    }
}
static void stmt_free(JSRuntime *rt, JSValue obj)
{
    Stmt *s = JS_GetOpaque(obj, stmt_id);
    if (s) {
        if (s->stmt)
            sqlite3_finalize(s->stmt);
        JS_FreeValueRT(rt, s->owner);
        free(s);
    }
}
static void stmt_mark(JSRuntime *rt, JSValueConst obj, JS_MarkFunc *mark)
{
    Stmt *s = JS_GetOpaque(obj, stmt_id);
    if (s)
        JS_MarkValue(rt, s->owner, mark);
}
static JSValue open_db(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv)
{
    (void)self;
    int flags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_URI;
    if (argc > 1 && JS_ToInt32(ctx, &flags, argv[1]))
        return JS_EXCEPTION;
    const char *name = argc ? JS_ToCString(ctx, argv[0]) : NULL;
    if (argc && !name)
        return JS_EXCEPTION;
    DB *d = calloc(1, sizeof(*d));
    if (!d) {
        JS_FreeCString(ctx, name);
        return JS_ThrowOutOfMemory(ctx);
    }
    int rc = sqlite3_open_v2(name ? name : ":memory:", &d->db, flags, NULL);
    JS_FreeCString(ctx, name);
    if (rc != SQLITE_OK) {
        JSValue e = fail(ctx, d->db, rc);
        sqlite3_close_v2(d->db);
        free(d);
        return e;
    }
    sqlite3_extended_result_codes(d->db, 1);
    d->safe = argc > 2 && JS_ToBool(ctx, argv[2]);
    JSValue obj = JS_NewObjectClass(ctx, db_id);
    if (JS_IsException(obj)) {
        sqlite3_close_v2(d->db);
        free(d);
        return obj;
    }
    JS_SetOpaque(obj, d);
    return obj;
}
static JSValue db_close(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv)
{
    DB *d = JS_GetOpaque2(ctx, self, db_id);
    if (!d)
        return JS_EXCEPTION;
    if (!d->db)
        return JS_UNDEFINED;
    int rc = argc && JS_ToBool(ctx, argv[0]) ? sqlite3_close(d->db) : sqlite3_close_v2(d->db);
    if (rc != SQLITE_OK)
        return fail(ctx, d->db, rc);
    d->db = NULL;
    return JS_UNDEFINED;
}
static JSValue db_exec(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv)
{
    DB *d = getdb(ctx, self);
    if (!d)
        return JS_EXCEPTION;
    const char *sql = JS_ToCString(ctx, argc ? argv[0] : JS_UNDEFINED);
    if (!sql)
        return JS_EXCEPTION;
    int rc = sqlite3_exec(d->db, sql, NULL, NULL, NULL);
    JS_FreeCString(ctx, sql);
    return rc == SQLITE_OK ? JS_UNDEFINED : fail(ctx, d->db, rc);
}
static JSValue db_prepare(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv)
{
    DB *d = getdb(ctx, self);
    if (!d)
        return JS_EXCEPTION;
    size_t len;
    const char *sql = JS_ToCStringLen(ctx, &len, argc ? argv[0] : JS_UNDEFINED);
    if (!sql)
        return JS_EXCEPTION;
    Stmt *s = calloc(1, sizeof(*s));
    if (!s) {
        JS_FreeCString(ctx, sql);
        return JS_ThrowOutOfMemory(ctx);
    }
    int rc = sqlite3_prepare_v3(d->db, sql, (int)len, SQLITE_PREPARE_PERSISTENT, &s->stmt, NULL);
    JS_FreeCString(ctx, sql);
    if (rc != SQLITE_OK || !s->stmt) {
        free(s);
        return rc != SQLITE_OK ? fail(ctx, d->db, rc)
                               : JS_ThrowTypeError(ctx, "SQL must contain a statement");
    }
    s->owner = JS_DupValue(ctx, self);
    s->safe = d->safe;
    s->strict = argc > 1 && JS_ToBool(ctx, argv[1]);
    s->bare = s->strict;
    JSValue obj = JS_NewObjectClass(ctx, stmt_id);
    if (JS_IsException(obj)) {
        sqlite3_finalize(s->stmt);
        JS_FreeValue(ctx, s->owner);
        free(s);
        return obj;
    }
    JS_SetOpaque(obj, s);
    return obj;
}
static int bind_value(JSContext *ctx, sqlite3_stmt *s, int i, JSValueConst v)
{
    int rc;
    if (JS_IsNull(v) || JS_IsUndefined(v))
        rc = sqlite3_bind_null(s, i);
    else if (JS_IsBool(v))
        rc = sqlite3_bind_int(s, i, JS_ToBool(ctx, v));
    else if (JS_IsBigInt(ctx, v)) {
        const char *str = JS_ToCString(ctx, v);
        if (!str)
            return -1;
        char *end;
        errno = 0;
        long long n = strtoll(str, &end, 10);
        int bad = errno == ERANGE || *end;
        JS_FreeCString(ctx, str);
        if (bad) {
            JS_ThrowRangeError(ctx, "BigInt must fit in a signed 64-bit integer");
            return -1;
        }
        rc = sqlite3_bind_int64(s, i, (sqlite3_int64)n);
    } else if (JS_IsNumber(v)) {
        double n;
        if (JS_ToFloat64(ctx, &n, v))
            return -1;
        rc =
            isfinite(n) && n >= -9223372036854775808.0 && n < 9223372036854775808.0 && trunc(n) == n
                ? sqlite3_bind_int64(s, i, (sqlite3_int64)n)
                : sqlite3_bind_double(s, i, n);
    } else if (JS_IsString(v)) {
        size_t len;
        const char *str = JS_ToCStringLen(ctx, &len, v);
        if (!str)
            return -1;
        rc = sqlite3_bind_text64(s, i, str, len, SQLITE_TRANSIENT, SQLITE_UTF8);
        JS_FreeCString(ctx, str);
    } else {
        size_t len;
        uint8_t *p = qb_get_bytes(ctx, v, &len);
        if (!p)
            return -1;
        rc = sqlite3_bind_blob64(s, i, p, len, SQLITE_TRANSIENT);
    }
    if (rc != SQLITE_OK) {
        fail(ctx, sqlite3_db_handle(s), rc);
        return -1;
    }
    return 0;
}
static int bind_params(JSContext *ctx, Stmt *s, JSValueConst params)
{
    sqlite3_reset(s->stmt);
    if (JS_IsUndefined(params))
        return 0;
    sqlite3_clear_bindings(s->stmt);
    int count = sqlite3_bind_parameter_count(s->stmt);
    if (JS_IsArray(ctx, params)) {
        JSValue lv = JS_GetPropertyStr(ctx, params, "length");
        uint32_t len = 0;
        int err = JS_ToUint32(ctx, &len, lv);
        JS_FreeValue(ctx, lv);
        if (err)
            return -1;
        if (len) {
            JSValue first = JS_GetPropertyUint32(ctx, params, 0);
            JSValue proto = JS_IsObject(first) ? JS_GetPrototype(ctx, first) : JS_UNDEFINED;
            JSValue ctor =
                JS_IsObject(proto) ? JS_GetPropertyStr(ctx, proto, "constructor") : JS_UNDEFINED;
            JSValue cname =
                JS_IsFunction(ctx, ctor) ? JS_GetPropertyStr(ctx, ctor, "name") : JS_UNDEFINED;
            const char *name = JS_IsString(cname) ? JS_ToCString(ctx, cname) : NULL;
            int named =
                JS_IsObject(first) && !JS_IsArray(ctx, first) && (!name || !strcmp(name, "Object"));
            JS_FreeCString(ctx, name);
            JS_FreeValue(ctx, cname);
            JS_FreeValue(ctx, ctor);
            JS_FreeValue(ctx, proto);
            if (named) {
                int r = bind_params(ctx, s, first);
                JS_FreeValue(ctx, first);
                if (r)
                    return -1;
                uint32_t k = 1;
                for (int j = 1; j <= count && k < len; j++) {
                    if (sqlite3_bind_parameter_name(s->stmt, j))
                        continue;
                    JSValue v = JS_GetPropertyUint32(ctx, params, k++);
                    r = bind_value(ctx, s->stmt, j, v);
                    JS_FreeValue(ctx, v);
                    if (r)
                        return -1;
                }
                if (k != len) {
                    JS_ThrowRangeError(ctx, "Too many positional SQL parameters");
                    return -1;
                }
                return 0;
            }
            JS_FreeValue(ctx, first);
        }
        if ((int)len > count || (s->strict && (int)len != count)) {
            JS_ThrowRangeError(ctx, "Incorrect number of SQL parameters");
            return -1;
        }
        for (uint32_t i = 0; i < len; i++) {
            JSValue v = JS_GetPropertyUint32(ctx, params, i);
            int r = bind_value(ctx, s->stmt, i + 1, v);
            JS_FreeValue(ctx, v);
            if (r)
                return -1;
        }
    } else if (JS_IsObject(params)) {
        JSPropertyEnum *tab;
        uint32_t len;
        if (JS_GetOwnPropertyNames(ctx, &tab, &len, params, JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY))
            return -1;
        int bad = 0;
        for (uint32_t k = 0; k < len; k++) {
            const char *key = JS_AtomToCString(ctx, tab[k].atom);
            int index = key ? sqlite3_bind_parameter_index(s->stmt, key) : 0;
            if (key && !index && s->bare && key[0] != '$' && key[0] != ':' && key[0] != '@') {
                for (int j = 1; j <= count; j++) {
                    const char *n = sqlite3_bind_parameter_name(s->stmt, j);
                    if (n && !strcmp(n + 1, key)) {
                        if (index) {
                            JS_ThrowTypeError(ctx, "Ambiguous named parameter: %s", key);
                            bad = 1;
                            break;
                        }
                        index = j;
                    }
                }
            }
            if (!index && !bad) {
                JS_ThrowRangeError(ctx, "Unknown SQL parameter: %s", key ? key : "");
                bad = 1;
            }
            JS_FreeCString(ctx, key);
            if (!bad) {
                JSValue v = JS_GetProperty(ctx, params, tab[k].atom);
                bad = bind_value(ctx, s->stmt, index, v) != 0;
                JS_FreeValue(ctx, v);
            }
            if (bad)
                break;
        }
        for (uint32_t k = 0; k < len; k++)
            JS_FreeAtom(ctx, tab[k].atom);
        js_free(ctx, tab);
        if (bad)
            return -1;
        if (s->strict)
            for (int j = 1; j <= count; j++) {
                const char *n = sqlite3_bind_parameter_name(s->stmt, j);
                if (!n)
                    continue;
                JSAtom atom = JS_NewAtom(ctx, n);
                int missing = !JS_HasProperty(ctx, params, atom);
                JS_FreeAtom(ctx, atom);
                if (missing && s->bare) {
                    atom = JS_NewAtom(ctx, n + 1);
                    missing = !JS_HasProperty(ctx, params, atom);
                    JS_FreeAtom(ctx, atom);
                }
                if (missing) {
                    JS_ThrowRangeError(ctx, "Missing SQL parameter: %s", n);
                    return -1;
                }
            }
    } else {
        JS_ThrowTypeError(ctx, "SQL parameters must be an array or object");
        return -1;
    }
    return 0;
}
static JSValue column(JSContext *ctx, sqlite3_stmt *s, int i, int safe)
{
    switch (sqlite3_column_type(s, i)) {
    case SQLITE_INTEGER:
        return safe ? JS_NewBigInt64(ctx, sqlite3_column_int64(s, i))
                    : JS_NewFloat64(ctx, (double)sqlite3_column_int64(s, i));
    case SQLITE_FLOAT:
        return JS_NewFloat64(ctx, sqlite3_column_double(s, i));
    case SQLITE_TEXT:
        return JS_NewStringLen(ctx, (const char *)sqlite3_column_text(s, i),
                               sqlite3_column_bytes(s, i));
    case SQLITE_BLOB:
        return qb_new_buffer(ctx, sqlite3_column_blob(s, i), sqlite3_column_bytes(s, i));
    default:
        return JS_NULL;
    }
}
static JSValue row(JSContext *ctx, Stmt *s, int arrays)
{
    JSValue r = arrays ? JS_NewArray(ctx) : JS_NewObject(ctx);
    if (JS_IsException(r))
        return r;
    for (int i = 0; i < sqlite3_column_count(s->stmt); i++) {
        JSValue v = column(ctx, s->stmt, i, s->safe);
        if (JS_IsException(v) ||
            (arrays ? JS_SetPropertyUint32(ctx, r, i, v)
                    : JS_DefinePropertyValueStr(ctx, r, sqlite3_column_name(s->stmt, i), v,
                                                JS_PROP_C_W_E)) < 0) {
            JS_FreeValue(ctx, r);
            return JS_EXCEPTION;
        }
    }
    return r;
}
/* mode: all, get, run, values, step-object, step-array. */
static JSValue stmt_query(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv, int mode)
{
    Stmt *s = getstmt(ctx, self);
    if (!s)
        return JS_EXCEPTION;
    if (mode < 4 && bind_params(ctx, s, argc ? argv[0] : JS_UNDEFINED))
        return JS_EXCEPTION;
    JSValue result = (mode == 0 || mode == 3) ? JS_NewArray(ctx) : JS_UNDEFINED;
    uint32_t n = 0;
    int rc;
    while ((rc = sqlite3_step(s->stmt)) == SQLITE_ROW) {
        if (mode == 2)
            continue;
        JSValue r = row(ctx, s, mode == 3 || mode == 5);
        if (JS_IsException(r)) {
            JS_FreeValue(ctx, result);
            sqlite3_reset(s->stmt);
            return r;
        }
        if (mode == 1 || mode >= 4) {
            if (mode == 1)
                sqlite3_reset(s->stmt);
            return r;
        }
        if (JS_SetPropertyUint32(ctx, result, n++, r) < 0) {
            JS_FreeValue(ctx, result);
            sqlite3_reset(s->stmt);
            return JS_EXCEPTION;
        }
    }
    if (rc != SQLITE_DONE) {
        JS_FreeValue(ctx, result);
        JSValue e = fail(ctx, sqlite3_db_handle(s->stmt), rc);
        sqlite3_reset(s->stmt);
        return e;
    }
    sqlite3_reset(s->stmt);
    if (mode == 1 || mode >= 4)
        return JS_NULL;
    if (mode == 2) {
        sqlite3 *db = sqlite3_db_handle(s->stmt);
        result = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, result, "changes",
                          JS_NewFloat64(ctx, (double)sqlite3_changes64(db)));
        JS_SetPropertyStr(ctx, result, "lastInsertRowid",
                          s->safe ? JS_NewBigInt64(ctx, sqlite3_last_insert_rowid(db))
                                  : JS_NewFloat64(ctx, (double)sqlite3_last_insert_rowid(db)));
    }
    return result;
}
static JSValue stmt_bind(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv)
{
    Stmt *s = getstmt(ctx, self);
    if (!s || bind_params(ctx, s, argc ? argv[0] : JS_UNDEFINED))
        return JS_EXCEPTION;
    return JS_UNDEFINED;
}
static JSValue stmt_action(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv,
                           int mode)
{
    Stmt *s = JS_GetOpaque2(ctx, self, stmt_id);
    if (!s)
        return JS_EXCEPTION;
    if (mode == 0) {
        if (s->stmt)
            sqlite3_finalize(s->stmt);
        s->stmt = NULL;
        return JS_UNDEFINED;
    }
    if (!getstmt(ctx, self))
        return JS_EXCEPTION;
    if (mode == 1)
        sqlite3_reset(s->stmt);
    if (mode == 2)
        s->safe = argc && JS_ToBool(ctx, argv[0]);
    if (mode == 3)
        s->bare = argc && JS_ToBool(ctx, argv[0]);
    return JS_UNDEFINED;
}
static JSValue stmt_string(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv)
{
    (void)argc;
    (void)argv;
    Stmt *s = getstmt(ctx, self);
    if (!s)
        return JS_EXCEPTION;
    char *sql = sqlite3_expanded_sql(s->stmt);
    if (!sql)
        return JS_ThrowOutOfMemory(ctx);
    JSValue v = JS_NewString(ctx, sql);
    sqlite3_free(sql);
    return v;
}
static JSValue stmt_readonly(JSContext *ctx, JSValueConst self)
{
    Stmt *s = getstmt(ctx, self);
    if (!s)
        return JS_EXCEPTION;
    return JS_NewBool(ctx, sqlite3_stmt_readonly(s->stmt));
}
static JSValue stmt_meta(JSContext *ctx, JSValueConst self, int mode)
{
    Stmt *s = getstmt(ctx, self);
    if (!s)
        return JS_EXCEPTION;
    if (mode == 0)
        return JS_NewInt32(ctx, sqlite3_bind_parameter_count(s->stmt));
    if (mode == 1) {
        char *sql = sqlite3_expanded_sql(s->stmt);
        JSValue v = JS_NewString(ctx, sql ? sql : "");
        sqlite3_free(sql);
        return v;
    }
    if (mode == 2)
        return JS_NewString(ctx, sqlite3_sql(s->stmt));
    JSValue a = JS_NewArray(ctx);
    for (int i = 0; i < sqlite3_column_count(s->stmt); i++) {
        const char *text = mode == 3   ? sqlite3_column_name(s->stmt, i)
                           : mode == 4 ? sqlite3_column_decltype(s->stmt, i)
                           : sqlite3_column_type(s->stmt, i) == SQLITE_INTEGER ? "INTEGER"
                           : sqlite3_column_type(s->stmt, i) == SQLITE_FLOAT   ? "FLOAT"
                           : sqlite3_column_type(s->stmt, i) == SQLITE_TEXT    ? "TEXT"
                           : sqlite3_column_type(s->stmt, i) == SQLITE_BLOB    ? "BLOB"
                                                                               : "NULL";
        JS_SetPropertyUint32(ctx, a, i, text ? JS_NewString(ctx, text) : JS_NULL);
    }
    return a;
}
static JSValue db_meta(JSContext *ctx, JSValueConst self, int mode)
{
    DB *d = JS_GetOpaque2(ctx, self, db_id);
    if (!d)
        return JS_EXCEPTION;
    if (mode == 0)
        return JS_NewBool(ctx, d->db != NULL);
    if (!getdb(ctx, self))
        return JS_EXCEPTION;
    if (mode == 1)
        return JS_NewBool(ctx, !sqlite3_get_autocommit(d->db));
    if (mode == 2)
        return JS_NewString(ctx, sqlite3_db_filename(d->db, "main"));
    if (mode == 5)
        return JS_NewFloat64(ctx, (double)(uintptr_t)d->db);
    if (mode == 3)
        return JS_NewFloat64(ctx, (double)sqlite3_changes64(d->db));
    return d->safe ? JS_NewBigInt64(ctx, sqlite3_last_insert_rowid(d->db))
                   : JS_NewFloat64(ctx, (double)sqlite3_last_insert_rowid(d->db));
}
static JSValue db_location(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv)
{
    DB *d = getdb(ctx, self);
    if (!d)
        return JS_EXCEPTION;
    const char *schema = argc ? JS_ToCString(ctx, argv[0]) : NULL;
    if (argc && !schema)
        return JS_EXCEPTION;
    const char *path = sqlite3_db_filename(d->db, schema ? schema : "main");
    JSValue v = path && *path ? JS_NewString(ctx, path) : JS_NULL;
    JS_FreeCString(ctx, schema);
    return v;
}
static JSValue db_dqs(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv)
{
    DB *d = getdb(ctx, self);
    if (!d)
        return JS_EXCEPTION;
    int on = argc && JS_ToBool(ctx, argv[0]);
    int rc = sqlite3_db_config(d->db, SQLITE_DBCONFIG_DQS_DML, on, NULL);
    if (rc == SQLITE_OK)
        rc = sqlite3_db_config(d->db, SQLITE_DBCONFIG_DQS_DDL, on, NULL);
    return rc == SQLITE_OK ? JS_UNDEFINED : fail(ctx, d->db, rc);
}
static JSValue stmt_columns(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv)
{
    (void)argc;
    (void)argv;
    Stmt *s = getstmt(ctx, self);
    if (!s)
        return JS_EXCEPTION;
    JSValue list = JS_NewArray(ctx);
    for (int i = 0; i < sqlite3_column_count(s->stmt); i++) {
        JSValue c = JS_NewObject(ctx);
        const char *name = sqlite3_column_name(s->stmt, i);
        const char *type = sqlite3_column_decltype(s->stmt, i);
#ifdef SQLITE_ENABLE_COLUMN_METADATA
        const char *origin = sqlite3_column_origin_name(s->stmt, i);
        const char *table = sqlite3_column_table_name(s->stmt, i);
        const char *database = sqlite3_column_database_name(s->stmt, i);
#else
        const char *origin = NULL, *table = NULL, *database = NULL;
#endif
        JS_SetPropertyStr(ctx, c, "name", JS_NewString(ctx, name));
        JS_SetPropertyStr(ctx, c, "type", type ? JS_NewString(ctx, type) : JS_NULL);
        JS_SetPropertyStr(ctx, c, "column", origin ? JS_NewString(ctx, origin) : JS_NULL);
        JS_SetPropertyStr(ctx, c, "table", table ? JS_NewString(ctx, table) : JS_NULL);
        JS_SetPropertyStr(ctx, c, "database", database ? JS_NewString(ctx, database) : JS_NULL);
        JS_SetPropertyUint32(ctx, list, i, c);
    }
    return list;
}
static JSValue db_serialize(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv)
{
    DB *d = getdb(ctx, self);
    if (!d)
        return JS_EXCEPTION;
    const char *schema = argc && !JS_IsUndefined(argv[0]) ? JS_ToCString(ctx, argv[0]) : NULL;
    sqlite3_int64 len = 0;
    unsigned char *p = sqlite3_serialize(d->db, schema ? schema : "main", &len, 0);
    JS_FreeCString(ctx, schema);
    if (!p)
        return fail(ctx, d->db, SQLITE_NOMEM);
    JSValue v = qb_new_buffer(ctx, p, len);
    sqlite3_free(p);
    return v;
}
static JSValue db_deserialize(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv)
{
    DB *d = getdb(ctx, self);
    if (!d)
        return JS_EXCEPTION;
    size_t len;
    uint8_t *p = qb_get_bytes(ctx, argc ? argv[0] : JS_UNDEFINED, &len);
    if (!p)
        return JS_EXCEPTION;
    unsigned char *copy = sqlite3_malloc64(len ? len : 1);
    if (!copy)
        return JS_ThrowOutOfMemory(ctx);
    memcpy(copy, p, len);
    unsigned flags = SQLITE_DESERIALIZE_FREEONCLOSE |
                     (argc > 1 && JS_ToBool(ctx, argv[1]) ? SQLITE_DESERIALIZE_READONLY
                                                          : SQLITE_DESERIALIZE_RESIZEABLE);
    int rc = sqlite3_deserialize(d->db, "main", copy, len, len, flags);
    if (rc != SQLITE_OK)
        return fail(ctx, d->db, rc);
    return JS_UNDEFINED;
}
static JSValue db_extension(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv)
{
    DB *d = getdb(ctx, self);
    if (!d)
        return JS_EXCEPTION;
    const char *path = JS_ToCString(ctx, argc ? argv[0] : JS_UNDEFINED);
    if (!path)
        return JS_EXCEPTION;
    const char *entry = argc > 1 && !JS_IsUndefined(argv[1]) ? JS_ToCString(ctx, argv[1]) : NULL;
    char *msg = NULL;
    int rc = sqlite3_load_extension(d->db, path, entry, &msg);
    JS_FreeCString(ctx, path);
    JS_FreeCString(ctx, entry);
    if (rc != SQLITE_OK) {
        JSValue e = JS_NewError(ctx);
        JS_SetPropertyStr(ctx, e, "message", JS_NewString(ctx, msg ? msg : sqlite3_errmsg(d->db)));
        JS_SetPropertyStr(ctx, e, "code", JS_NewString(ctx, error_name(rc)));
        JS_SetPropertyStr(ctx, e, "errno", JS_NewInt32(ctx, rc));
        sqlite3_free(msg);
        return JS_Throw(ctx, e);
    }
    return JS_UNDEFINED;
}
static JSValue db_control(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv, int mode)
{
    DB *d = getdb(ctx, self);
    if (!d)
        return JS_EXCEPTION;
    int n = 0;
    if (argc && JS_ToInt32(ctx, &n, argv[0]))
        return JS_EXCEPTION;
    int rc;
    if (mode == 0)
        rc = sqlite3_busy_timeout(d->db, n);
    else if (mode == 1)
        rc = sqlite3_enable_load_extension(d->db, argc && JS_ToBool(ctx, argv[0]));
    else {
        /* Pointer-valued controls require a real byte view; integer controls use int64 storage. */
        sqlite3_int64 value = 0;
        void *ptr = &value;
        if (argc > 1 && JS_IsObject(argv[1])) {
            size_t len;
            ptr = qb_get_bytes(ctx, argv[1], &len);
            if (!ptr)
                return JS_EXCEPTION;
            if (len < sizeof(sqlite3_int64))
                return JS_ThrowRangeError(ctx, "fileControl buffer must have at least 8 bytes");
        } else if (argc > 1 && !JS_IsNull(argv[1]) && JS_ToInt64Ext(ctx, &value, argv[1]))
            return JS_EXCEPTION;
        const char *schema =
            argc > 2 && !JS_IsUndefined(argv[2]) ? JS_ToCString(ctx, argv[2]) : NULL;
        if (argc > 2 && !JS_IsUndefined(argv[2]) && !schema)
            return JS_EXCEPTION;
        rc = sqlite3_file_control(d->db, schema ? schema : "main", n, ptr);
        JS_FreeCString(ctx, schema);
        return JS_NewInt32(ctx, rc);
    }
    return rc == SQLITE_OK ? JS_UNDEFINED : fail(ctx, d->db, rc);
}
/* The database's callback table owns JS references, making closure cycles visible to GC. */
static void func_free(void *opaque)
{
    free(opaque);
}
static void func_call(sqlite3_context *sqlctx, int argc, sqlite3_value **values)
{
    Func *f = sqlite3_user_data(sqlctx);
    JSContext *ctx = f->ctx;
    JSValue *args = js_malloc(ctx, sizeof(*args) * (argc ? argc : 1));
    if (!args) {
        sqlite3_result_error_nomem(sqlctx);
        return;
    }
    for (int i = 0; i < argc; i++) {
        switch (sqlite3_value_type(values[i])) {
        case SQLITE_INTEGER:
            args[i] = f->safe ? JS_NewBigInt64(ctx, sqlite3_value_int64(values[i]))
                              : JS_NewFloat64(ctx, (double)sqlite3_value_int64(values[i]));
            break;
        case SQLITE_FLOAT:
            args[i] = JS_NewFloat64(ctx, sqlite3_value_double(values[i]));
            break;
        case SQLITE_TEXT:
            args[i] = JS_NewStringLen(ctx, (const char *)sqlite3_value_text(values[i]),
                                      sqlite3_value_bytes(values[i]));
            break;
        case SQLITE_BLOB:
            args[i] =
                qb_new_buffer(ctx, sqlite3_value_blob(values[i]), sqlite3_value_bytes(values[i]));
            break;
        default:
            args[i] = JS_NULL;
        }
    }
    JSValue result = JS_Call(ctx, f->callback, JS_UNDEFINED, argc, args);
    for (int i = 0; i < argc; i++)
        JS_FreeValue(ctx, args[i]);
    js_free(ctx, args);
    if (JS_IsException(result)) {
        JSValue e = JS_GetException(ctx);
        const char *msg = JS_ToCString(ctx, e);
        sqlite3_result_error(sqlctx, msg ? msg : "User function failed", -1);
        JS_FreeCString(ctx, msg);
        JS_FreeValue(ctx, e);
    } else if (JS_IsNull(result) || JS_IsUndefined(result))
        sqlite3_result_null(sqlctx);
    else if (JS_IsBigInt(ctx, result)) {
        const char *text = JS_ToCString(ctx, result);
        if (!text) {
            JSValue e = JS_GetException(ctx);
            JS_FreeValue(ctx, e);
            sqlite3_result_error_nomem(sqlctx);
        } else {
            char *end;
            errno = 0;
            long long n = strtoll(text, &end, 10);
            if (errno == ERANGE || *end)
                sqlite3_result_error(sqlctx, "BigInt result must fit in a signed 64-bit integer",
                                     -1);
            else
                sqlite3_result_int64(sqlctx, n);
            JS_FreeCString(ctx, text);
        }
    } else if (JS_IsBool(result))
        sqlite3_result_int(sqlctx, JS_ToBool(ctx, result));
    else if (JS_IsNumber(result)) {
        double n;
        JS_ToFloat64(ctx, &n, result);
        sqlite3_result_double(sqlctx, n);
    } else if (JS_IsString(result)) {
        size_t len;
        const char *p = JS_ToCStringLen(ctx, &len, result);
        if (p) {
            sqlite3_result_text64(sqlctx, p, len, SQLITE_TRANSIENT, SQLITE_UTF8);
            JS_FreeCString(ctx, p);
        } else
            sqlite3_result_error_nomem(sqlctx);
    } else {
        size_t len;
        uint8_t *p = qb_get_bytes(ctx, result, &len);
        if (p)
            sqlite3_result_blob64(sqlctx, p, len, SQLITE_TRANSIENT);
        else {
            JSValue e = JS_GetException(ctx);
            JS_FreeValue(ctx, e);
            sqlite3_result_error(sqlctx, "Invalid user function result", -1);
        }
    }
    JS_FreeValue(ctx, result);
}
static JSValue db_function(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv)
{
    DB *d = getdb(ctx, self);
    if (!d)
        return JS_EXCEPTION;
    if (argc < 3 || !JS_IsFunction(ctx, argv[2]))
        return JS_ThrowTypeError(ctx, "Expected a function");
    const char *name = JS_ToCString(ctx, argv[0]);
    if (!name)
        return JS_EXCEPTION;
    Func *f = calloc(1, sizeof(*f));
    if (!f) {
        JS_FreeCString(ctx, name);
        return JS_ThrowOutOfMemory(ctx);
    }
    f->ctx = ctx;
    f->callback = argv[2];
    JSValue v = JS_GetPropertyStr(ctx, argv[1], "useBigIntArguments");
    f->safe = JS_ToBool(ctx, v);
    JS_FreeValue(ctx, v);
    v = JS_GetPropertyStr(ctx, argv[1], "varargs");
    int varargs = JS_ToBool(ctx, v);
    JS_FreeValue(ctx, v);
    v = JS_GetPropertyStr(ctx, argv[2], "length");
    int arity = 0;
    JS_ToInt32(ctx, &arity, v);
    JS_FreeValue(ctx, v);
    int flags = SQLITE_UTF8;
    v = JS_GetPropertyStr(ctx, argv[1], "deterministic");
    if (JS_ToBool(ctx, v))
        flags |= SQLITE_DETERMINISTIC;
    JS_FreeValue(ctx, v);
    v = JS_GetPropertyStr(ctx, argv[1], "directOnly");
    if (JS_ToBool(ctx, v))
        flags |= SQLITE_DIRECTONLY;
    JS_FreeValue(ctx, v);
    JSValue roots = JS_GetPropertyStr(ctx, self, "__sqliteFunctions");
    if (JS_IsUndefined(roots)) {
        JS_FreeValue(ctx, roots);
        roots = JS_NewArray(ctx);
        if (JS_IsException(roots) ||
            JS_DefinePropertyValueStr(ctx, self, "__sqliteFunctions", JS_DupValue(ctx, roots),
                                      JS_PROP_CONFIGURABLE) < 0) {
            JS_FreeValue(ctx, roots);
            JS_FreeCString(ctx, name);
            free(f);
            return JS_EXCEPTION;
        }
    }
    JSValue length = JS_GetPropertyStr(ctx, roots, "length");
    uint32_t index = 0;
    int length_error = JS_ToUint32(ctx, &index, length);
    JS_FreeValue(ctx, length);
    if (length_error) {
        JS_FreeValue(ctx, roots);
        JS_FreeCString(ctx, name);
        free(f);
        return JS_EXCEPTION;
    }
    int rooted = JS_SetPropertyUint32(ctx, roots, index, JS_DupValue(ctx, argv[2]));
    JS_FreeValue(ctx, roots);
    if (rooted < 0) {
        JS_FreeCString(ctx, name);
        free(f);
        return JS_EXCEPTION;
    }
    int rc = sqlite3_create_function_v2(d->db, name, varargs ? -1 : arity, flags, f, func_call,
                                        NULL, NULL, func_free);
    JS_FreeCString(ctx, name);
    return rc == SQLITE_OK ? JS_UNDEFINED : fail(ctx, d->db, rc);
}
static const JSCFunctionListEntry db_methods[] = {
    JS_CFUNC_DEF("close", 1, db_close),
    JS_CFUNC_DEF("exec", 1, db_exec),
    JS_CFUNC_DEF("prepare", 2, db_prepare),
    JS_CFUNC_DEF("location", 1, db_location),
    JS_CFUNC_DEF("configureDoubleQuotedStrings", 1, db_dqs),
    JS_CFUNC_DEF("serialize", 1, db_serialize),
    JS_CFUNC_DEF("deserialize", 2, db_deserialize),
    JS_CFUNC_DEF("loadExtension", 2, db_extension),
    JS_CFUNC_DEF("function", 3, db_function),
    JS_CFUNC_MAGIC_DEF("busyTimeout", 1, db_control, 0),
    JS_CFUNC_MAGIC_DEF("enableLoadExtension", 1, db_control, 1),
    JS_CFUNC_MAGIC_DEF("fileControl", 2, db_control, 2),
    JS_CGETSET_MAGIC_DEF("isOpen", db_meta, NULL, 0),
    JS_CGETSET_MAGIC_DEF("inTransaction", db_meta, NULL, 1),
    JS_CGETSET_MAGIC_DEF("filename", db_meta, NULL, 2),
    JS_CGETSET_MAGIC_DEF("changes", db_meta, NULL, 3),
    JS_CGETSET_MAGIC_DEF("lastInsertRowid", db_meta, NULL, 4),
    JS_CGETSET_MAGIC_DEF("address", db_meta, NULL, 5),
};
static const JSCFunctionListEntry stmt_methods[] = {
    JS_CFUNC_MAGIC_DEF("all", 1, stmt_query, 0),
    JS_CFUNC_MAGIC_DEF("get", 1, stmt_query, 1),
    JS_CFUNC_MAGIC_DEF("run", 1, stmt_query, 2),
    JS_CFUNC_MAGIC_DEF("values", 1, stmt_query, 3),
    JS_CFUNC_MAGIC_DEF("step", 0, stmt_query, 4),
    JS_CFUNC_MAGIC_DEF("stepArray", 0, stmt_query, 5),
    JS_CFUNC_DEF("bind", 1, stmt_bind),
    JS_CFUNC_DEF("columns", 0, stmt_columns),
    JS_CFUNC_DEF("toString", 0, stmt_string),
    JS_CGETSET_DEF("readonly", stmt_readonly, NULL),
    JS_CFUNC_MAGIC_DEF("finalize", 0, stmt_action, 0),
    JS_CFUNC_MAGIC_DEF("reset", 0, stmt_action, 1),
    JS_CFUNC_MAGIC_DEF("setReadBigInts", 1, stmt_action, 2),
    JS_CFUNC_MAGIC_DEF("setAllowBareNamedParameters", 1, stmt_action, 3),
    JS_CGETSET_MAGIC_DEF("paramsCount", stmt_meta, NULL, 0),
    JS_CGETSET_MAGIC_DEF("expandedSQL", stmt_meta, NULL, 1),
    JS_CGETSET_MAGIC_DEF("sourceSQL", stmt_meta, NULL, 2),
    JS_CGETSET_MAGIC_DEF("columnNames", stmt_meta, NULL, 3),
    JS_CGETSET_MAGIC_DEF("declaredTypes", stmt_meta, NULL, 4),
    JS_CGETSET_MAGIC_DEF("columnTypes", stmt_meta, NULL, 5),
};
static int module_init(JSContext *ctx, JSModuleDef *m)
{
    pthread_once(&ids_once, init_ids);
    JSRuntime *rt = JS_GetRuntime(ctx);
    if (!JS_IsRegisteredClass(rt, db_id)) {
        JSClassDef c = {"SQLiteDatabase", .finalizer = db_free};
        if (JS_NewClass(rt, db_id, &c) < 0)
            return -1;
    }
    if (!JS_IsRegisteredClass(rt, stmt_id)) {
        JSClassDef c = {"SQLiteStatement", .finalizer = stmt_free, .gc_mark = stmt_mark};
        if (JS_NewClass(rt, stmt_id, &c) < 0)
            return -1;
    }
    JSValue p = JS_NewObject(ctx);
    JS_SetPropertyFunctionList(ctx, p, db_methods, countof(db_methods));
    JS_SetClassProto(ctx, db_id, p);
    p = JS_NewObject(ctx);
    JS_SetPropertyFunctionList(ctx, p, stmt_methods, countof(stmt_methods));
    JS_SetClassProto(ctx, stmt_id, p);
    return JS_SetModuleExport(ctx, m, "open", JS_NewCFunction(ctx, open_db, "open", 3));
}
JSModuleDef *qb_init_sqlite(JSContext *ctx, const char *name)
{
    JSModuleDef *m = JS_NewCModule(ctx, name, module_init);
    if (m)
        JS_AddModuleExport(ctx, m, "open");
    return m;
}
