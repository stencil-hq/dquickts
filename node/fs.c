/*
 * qbun:fs — file system natives on libuv uv_fs_*.
 *
 * Every operation runs synchronously (it returns its result or throws a
 * Node-style system error) unless its last argument is a function: then
 * the request runs on the libuv thread pool and the callback receives
 * (err, result) through qb_call().
 *
 * Results:
 *   stats    an array of 18 numbers (BigInts when 'bigint'):
 *            dev mode nlink uid gid rdev blksize ino size blocks
 *            atimeSec atimeNsec mtimeSec mtimeNsec ctimeSec ctimeNsec
 *            birthtimeSec birthtimeNsec
 *   statfs   [type, bsize, blocks, bfree, bavail, files, ffree]
 *   scandir  [names[], types[]] (types: UV_DIRENT_*)
 *   opendir  a Dir handle: read(n, cb?) → [names, types] | null, close(cb?)
 */
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "qbun.h"

static JSClassID qb_dir_class_id;
static JSClassID qb_fswatch_class_id;

/* ------------------------------------------------------------ requests */

typedef struct QBFsReq {
    uv_fs_t req;
    JSContext *ctx;
    uv_loop_t *loop;
    int async;
    int bigint;
    JSValue cb;
    JSValue keep; /* buffers / Dir object kept alive */
    const char *syscall;
    char *path;
    char *dest;
    uv_buf_t *bufs; /* readv/writev */
    uv_dirent_t *dirents;
} QBFsReq;

static void fs_after(uv_fs_t *req);

static QBFsReq *fs_req_new(JSContext *ctx, int argc, JSValueConst *argv, const char *syscall)
{
    QBFsReq *r = calloc(1, sizeof(*r));
    if (!r) {
        JS_ThrowOutOfMemory(ctx);
        return NULL;
    }
    r->ctx = ctx;
    r->loop = qb_get(ctx)->loop;
    r->syscall = syscall;
    r->cb = JS_UNDEFINED;
    r->keep = JS_UNDEFINED;
    if (argc > 0 && JS_IsFunction(ctx, argv[argc - 1])) {
        r->async = 1;
        r->cb = JS_DupValue(ctx, argv[argc - 1]);
    }
    r->req.data = r;
    return r;
}

static void fs_req_free(QBFsReq *r)
{
    JSContext *ctx = r->ctx;
    JS_FreeValue(ctx, r->cb);
    JS_FreeValue(ctx, r->keep);
    free(r->path);
    free(r->dest);
    free(r->bufs);
    free(r->dirents);
    free(r);
}

#define FS_CB(r) ((r)->async ? fs_after : NULL)

/* a malloc'd C path from a string or bytes (Buffer path) */
static char *fs_cpath(JSContext *ctx, JSValueConst val)
{
    char *p;
    if (JS_IsString(val)) {
        size_t len;
        const char *s = JS_ToCStringLen(ctx, &len, val);
        if (!s)
            return NULL;
        if (memchr(s, 0, len)) {
            JS_FreeCString(ctx, s);
            qb_throw_code(
                ctx, JS_TYPE_ERROR, "ERR_INVALID_ARG_VALUE",
                "The argument 'path' must be a string, Uint8Array, or URL without null bytes.");
            return NULL;
        }
        p = strdup(s);
        JS_FreeCString(ctx, s);
    } else if (JS_IsObject(val)) {
        size_t len;
        uint8_t *b = qb_get_bytes(ctx, val, &len);
        if (!b)
            return NULL;
        if (memchr(b, 0, len)) {
            qb_throw_code(
                ctx, JS_TYPE_ERROR, "ERR_INVALID_ARG_VALUE",
                "The argument 'path' must be a string, Uint8Array, or URL without null bytes.");
            return NULL;
        }
        p = malloc(len + 1);
        if (p) {
            memcpy(p, b, len);
            p[len] = '\0';
        }
    } else {
        qb_throw_code(
            ctx, JS_TYPE_ERROR, "ERR_INVALID_ARG_TYPE",
            "The \"path\" argument must be of type string or an instance of Buffer or URL.");
        return NULL;
    }
    if (!p)
        JS_ThrowOutOfMemory(ctx);
    return p;
}

static int fs_int(JSContext *ctx, int *pres, JSValueConst v, int def)
{
    if (JS_IsUndefined(v) || JS_IsNull(v)) {
        *pres = def;
        return 0;
    }
    return JS_ToInt32(ctx, pres, v);
}

static int fs_i64(JSContext *ctx, int64_t *pres, JSValueConst v, int64_t def)
{
    if (JS_IsUndefined(v) || JS_IsNull(v)) {
        *pres = def;
        return 0;
    }
    if (JS_IsNumber(v)) {
        double d;
        if (JS_ToFloat64(ctx, &d, v))
            return -1;
        *pres = d != d ? def : (int64_t)d;
        return 0;
    }
    return JS_ToInt64Ext(ctx, pres, v);
}

static JSValue fs_num(JSContext *ctx, int bigint, int64_t v)
{
    return bigint ? JS_NewBigInt64(ctx, v) : JS_NewFloat64(ctx, (double)v);
}

static JSValue fs_unum(JSContext *ctx, int bigint, uint64_t v)
{
    return bigint ? JS_NewBigUint64(ctx, v) : JS_NewFloat64(ctx, (double)v);
}

static JSValue fs_stats(JSContext *ctx, const uv_stat_t *s, int bigint)
{
    JSValue v[18];
    v[0] = fs_unum(ctx, bigint, s->st_dev);
    v[1] = fs_unum(ctx, bigint, s->st_mode);
    v[2] = fs_unum(ctx, bigint, s->st_nlink);
    v[3] = fs_unum(ctx, bigint, s->st_uid);
    v[4] = fs_unum(ctx, bigint, s->st_gid);
    v[5] = fs_unum(ctx, bigint, s->st_rdev);
    v[6] = fs_unum(ctx, bigint, s->st_blksize);
    v[7] = fs_unum(ctx, bigint, s->st_ino);
    v[8] = fs_unum(ctx, bigint, s->st_size);
    v[9] = fs_unum(ctx, bigint, s->st_blocks);
    v[10] = fs_num(ctx, bigint, s->st_atim.tv_sec);
    v[11] = fs_num(ctx, bigint, s->st_atim.tv_nsec);
    v[12] = fs_num(ctx, bigint, s->st_mtim.tv_sec);
    v[13] = fs_num(ctx, bigint, s->st_mtim.tv_nsec);
    v[14] = fs_num(ctx, bigint, s->st_ctim.tv_sec);
    v[15] = fs_num(ctx, bigint, s->st_ctim.tv_nsec);
    v[16] = fs_num(ctx, bigint, s->st_birthtim.tv_sec);
    v[17] = fs_num(ctx, bigint, s->st_birthtim.tv_nsec);
    return JS_NewArrayFrom(ctx, 18, v);
}

static JSValue fs_entries(JSContext *ctx, uv_dirent_t *ents, size_t n, uv_fs_t *scan)
{
    JSValue names = JS_NewArray(ctx), types = JS_NewArray(ctx), pair[2];
    uint32_t i = 0;
    uv_dirent_t ent;
    if (JS_IsException(names) || JS_IsException(types))
        goto fail;
    for (;;) {
        const char *name;
        int type;
        if (scan) {
            int rc = uv_fs_scandir_next(scan, &ent);
            if (rc == UV_EOF || rc < 0)
                break;
            name = ent.name;
            type = ent.type;
        } else {
            if (i >= n)
                break;
            name = ents[i].name;
            type = ents[i].type;
        }
        if (JS_SetPropertyUint32(ctx, names, i, JS_NewString(ctx, name)) < 0 ||
            JS_SetPropertyUint32(ctx, types, i, JS_NewInt32(ctx, type)) < 0)
            goto fail;
        i++;
    }
    pair[0] = names;
    pair[1] = types;
    return JS_NewArrayFrom(ctx, 2, pair);
fail:
    JS_FreeValue(ctx, names);
    JS_FreeValue(ctx, types);
    return JS_EXCEPTION;
}

typedef struct QBDir {
    uv_dir_t *dir;
    uv_loop_t *loop;
    int busy;
} QBDir;

static JSValue fs_new_dir(JSContext *ctx, uv_dir_t *dir, uv_loop_t *loop)
{
    JSValue obj = JS_NewObjectClass(ctx, qb_dir_class_id);
    QBDir *d;
    if (JS_IsException(obj))
        goto fail;
    d = calloc(1, sizeof(*d));
    if (!d) {
        JS_FreeValue(ctx, obj);
        JS_ThrowOutOfMemory(ctx);
        goto fail;
    }
    d->dir = dir;
    d->loop = loop;
    JS_SetOpaque(obj, d);
    return obj;
fail:
    {
        uv_fs_t req;
        uv_fs_closedir(loop, &req, dir, NULL);
        uv_fs_req_cleanup(&req);
    }
    return JS_EXCEPTION;
}

/* the result of a finished request; *is_err set when it failed (the
   value is then the error) */
static JSValue fs_result(JSContext *ctx, QBFsReq *r, int *is_err)
{
    uv_fs_t *req = &r->req;
    ssize_t res = req->result;
    *is_err = 0;
    if (res < 0) {
        *is_err = 1;
        if (uv_fs_get_type(req) == UV_FS_CLOSEDIR || uv_fs_get_type(req) == UV_FS_READDIR) {
            QBDir *d = JS_GetOpaque(r->keep, qb_dir_class_id);
            if (d)
                d->busy = 0;
        }
        return qb_uv_error(ctx, (int)res, r->syscall, r->path, r->dest);
    }
    switch (uv_fs_get_type(req)) {
    case UV_FS_OPEN:
    case UV_FS_READ:
    case UV_FS_WRITE:
    case UV_FS_SENDFILE:
        return JS_NewInt64(ctx, res);
    case UV_FS_STAT:
    case UV_FS_LSTAT:
    case UV_FS_FSTAT:
        return fs_stats(ctx, &req->statbuf, r->bigint);
    case UV_FS_STATFS: {
        uv_statfs_t *s = req->ptr;
        JSValue v[7];
        v[0] = fs_unum(ctx, r->bigint, s->f_type);
        v[1] = fs_unum(ctx, r->bigint, s->f_bsize);
        v[2] = fs_unum(ctx, r->bigint, s->f_blocks);
        v[3] = fs_unum(ctx, r->bigint, s->f_bfree);
        v[4] = fs_unum(ctx, r->bigint, s->f_bavail);
        v[5] = fs_unum(ctx, r->bigint, s->f_files);
        v[6] = fs_unum(ctx, r->bigint, s->f_ffree);
        return JS_NewArrayFrom(ctx, 7, v);
    }
    case UV_FS_SCANDIR:
        return fs_entries(ctx, NULL, 0, req);
    case UV_FS_READLINK:
    case UV_FS_REALPATH:
        return JS_NewString(ctx, req->ptr);
    case UV_FS_MKDTEMP:
        return JS_NewString(ctx, req->path);
    case UV_FS_OPENDIR:
        return fs_new_dir(ctx, req->ptr, r->loop);
    case UV_FS_READDIR: {
        QBDir *d = JS_GetOpaque(r->keep, qb_dir_class_id);
        uv_dir_t *dir = req->ptr;
        if (d)
            d->busy = 0;
        if (res == 0)
            return JS_NULL;
        return fs_entries(ctx, dir->dirents, (size_t)res, NULL);
    }
    case UV_FS_CLOSEDIR: {
        QBDir *d = JS_GetOpaque(r->keep, qb_dir_class_id);
        if (d) {
            d->busy = 0;
            d->dir = NULL;
        }
        return JS_UNDEFINED;
    }
    default:
        return JS_UNDEFINED;
    }
}

static void fs_after(uv_fs_t *req)
{
    QBFsReq *r = req->data;
    JSContext *ctx = r->ctx;
    JSValue args[2], cb, ret;
    int is_err;
    JSValue res = fs_result(ctx, r, &is_err);
    if (JS_IsException(res)) {
        args[0] = JS_GetException(ctx);
        args[1] = JS_UNDEFINED;
    } else if (is_err) {
        args[0] = res;
        args[1] = JS_UNDEFINED;
    } else {
        args[0] = JS_NULL;
        args[1] = res;
    }
    cb = r->cb;
    r->cb = JS_UNDEFINED;
    uv_fs_req_cleanup(req);
    fs_req_free(r);
    ret = qb_call(ctx, cb, JS_UNDEFINED, 2, (JSValueConst *)args);
    JS_FreeValue(ctx, ret);
    JS_FreeValue(ctx, cb);
    JS_FreeValue(ctx, args[0]);
    JS_FreeValue(ctx, args[1]);
}

/* finish a submitted request: rc is the return of uv_fs_*() */
static JSValue fs_finish(JSContext *ctx, QBFsReq *r, int rc)
{
    JSValue res;
    int is_err;
    if (r->async) {
        if (rc >= 0)
            return JS_UNDEFINED;
        /* not submitted: report synchronously through the callback
           contract (Node throws for invalid arguments here) */
        res = qb_uv_error(ctx, rc, r->syscall, r->path, r->dest);
        uv_fs_req_cleanup(&r->req);
        fs_req_free(r);
        return JS_Throw(ctx, res);
    }
    res = fs_result(ctx, r, &is_err);
    uv_fs_req_cleanup(&r->req);
    fs_req_free(r);
    if (is_err)
        return JS_Throw(ctx, res);
    return res;
}

static JSValue fs_fail(QBFsReq *r)
{
    fs_req_free(r);
    return JS_EXCEPTION;
}

/* ------------------------------------------------------------ operations */

/* open(path, flags, mode, cb?) → fd */
static JSValue qb_fs_open(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    QBFsReq *r = fs_req_new(ctx, argc, argv, "open");
    int flags, mode;
    if (!r)
        return JS_EXCEPTION;
    if (!(r->path = fs_cpath(ctx, argv[0])) || fs_int(ctx, &flags, argv[1], O_RDONLY) ||
        fs_int(ctx, &mode, argv[2], 0666))
        return fs_fail(r);
    return fs_finish(ctx, r, uv_fs_open(r->loop, &r->req, r->path, flags, mode, FS_CB(r)));
}

/* fd-only ops: close, fsync, fdatasync (magic) */
static JSValue qb_fs_fdop(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv,
                          int magic)
{
    static const char *const names[] = {"close", "fsync", "fdatasync"};
    QBFsReq *r = fs_req_new(ctx, argc, argv, names[magic]);
    int fd, rc;
    if (!r)
        return JS_EXCEPTION;
    if (JS_ToInt32(ctx, &fd, argv[0]))
        return fs_fail(r);
    switch (magic) {
    case 0:
        rc = uv_fs_close(r->loop, &r->req, fd, FS_CB(r));
        break;
    case 1:
        rc = uv_fs_fsync(r->loop, &r->req, fd, FS_CB(r));
        break;
    default:
        rc = uv_fs_fdatasync(r->loop, &r->req, fd, FS_CB(r));
        break;
    }
    return fs_finish(ctx, r, rc);
}

/* read/write(fd, buffer, offset, length, position, cb?) (magic 1 = write) */
static JSValue qb_fs_rw(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv,
                        int magic)
{
    QBFsReq *r = fs_req_new(ctx, argc, argv, magic ? "write" : "read");
    int fd;
    int64_t off, len, pos;
    size_t blen;
    uint8_t *b;
    uv_buf_t buf;
    int rc;
    if (!r)
        return JS_EXCEPTION;
    if (JS_ToInt32(ctx, &fd, argv[0]))
        return fs_fail(r);
    b = qb_get_bytes(ctx, argv[1], &blen);
    if (!b && blen == 0 && JS_HasException(ctx))
        return fs_fail(r);
    if (fs_i64(ctx, &off, argv[2], 0) || fs_i64(ctx, &len, argv[3], (int64_t)blen - off) ||
        fs_i64(ctx, &pos, argv[4], -1))
        return fs_fail(r);
    if (off < 0 || (uint64_t)off > blen || len < 0 || (uint64_t)(off + len) > blen) {
        fs_req_free(r);
        return qb_throw_code(ctx, JS_RANGE_ERROR, "ERR_OUT_OF_RANGE",
                             "The value of \"length\" is out of range.");
    }
    if (len > INT32_MAX)
        len = INT32_MAX;
    if (pos < 0)
        pos = -1;
    r->keep = JS_DupValue(ctx, argv[1]);
    buf = uv_buf_init((char *)(b ? b + off : (uint8_t *)""), (unsigned int)len);
    if (magic)
        rc = uv_fs_write(r->loop, &r->req, fd, &buf, 1, pos, FS_CB(r));
    else
        rc = uv_fs_read(r->loop, &r->req, fd, &buf, 1, pos, FS_CB(r));
    return fs_finish(ctx, r, rc);
}

/* readv/writev(fd, buffers[], position, cb?) (magic 1 = writev) */
static JSValue qb_fs_rwv(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv,
                         int magic)
{
    QBFsReq *r = fs_req_new(ctx, argc, argv, magic ? "write" : "read");
    int fd, rc;
    int64_t n, pos, i;
    if (!r)
        return JS_EXCEPTION;
    if (JS_ToInt32(ctx, &fd, argv[0]) || JS_GetLength(ctx, argv[1], &n) ||
        fs_i64(ctx, &pos, argv[2], -1))
        return fs_fail(r);
    if (pos < 0)
        pos = -1;
    r->bufs = calloc(n > 0 ? (size_t)n : 1, sizeof(uv_buf_t));
    if (!r->bufs) {
        fs_req_free(r);
        return JS_ThrowOutOfMemory(ctx);
    }
    r->keep = JS_DupValue(ctx, argv[1]);
    for (i = 0; i < n; i++) {
        JSValue v = JS_GetPropertyUint32(ctx, argv[1], (uint32_t)i);
        size_t blen;
        uint8_t *b = qb_get_bytes(ctx, v, &blen);
        JS_FreeValue(ctx, v); /* still referenced by the kept array */
        if (!b && JS_HasException(ctx))
            return fs_fail(r);
        r->bufs[i] = uv_buf_init((char *)(b ? b : (uint8_t *)""), (unsigned int)blen);
    }
    if (magic)
        rc = uv_fs_write(r->loop, &r->req, fd, r->bufs, (unsigned int)n, pos, FS_CB(r));
    else
        rc = uv_fs_read(r->loop, &r->req, fd, r->bufs, (unsigned int)n, pos, FS_CB(r));
    return fs_finish(ctx, r, rc);
}

/* stat/lstat(path, bigint, cb?), statfs (magic 2) */
static JSValue qb_fs_stat(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv,
                          int magic)
{
    static const char *const names[] = {"stat", "lstat", "statfs"};
    QBFsReq *r = fs_req_new(ctx, argc, argv, names[magic]);
    int rc;
    if (!r)
        return JS_EXCEPTION;
    if (!(r->path = fs_cpath(ctx, argv[0])))
        return fs_fail(r);
    r->bigint = argc > 1 && JS_ToBool(ctx, argv[1]) > 0;
    switch (magic) {
    case 0:
        rc = uv_fs_stat(r->loop, &r->req, r->path, FS_CB(r));
        break;
    case 1:
        rc = uv_fs_lstat(r->loop, &r->req, r->path, FS_CB(r));
        break;
    default:
        rc = uv_fs_statfs(r->loop, &r->req, r->path, FS_CB(r));
        break;
    }
    return fs_finish(ctx, r, rc);
}

/* fstat(fd, bigint, cb?) */
static JSValue qb_fs_fstat(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    QBFsReq *r = fs_req_new(ctx, argc, argv, "fstat");
    int fd;
    if (!r)
        return JS_EXCEPTION;
    if (JS_ToInt32(ctx, &fd, argv[0]))
        return fs_fail(r);
    r->bigint = argc > 1 && JS_ToBool(ctx, argv[1]) > 0;
    return fs_finish(ctx, r, uv_fs_fstat(r->loop, &r->req, fd, FS_CB(r)));
}

/* path ops with an int argument: access(path, mode), mkdir(path, mode),
   chmod(path, mode); and path-only: rmdir, unlink, readlink, realpath,
   scandir, opendir, mkdtemp(prefix) */
static JSValue qb_fs_pathop(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv,
                            int magic)
{
    static const char *const names[] = {
        "access",   "mkdir",    "chmod",   "rmdir",   "unlink",
        "readlink", "realpath", "scandir", "opendir", "mkdtemp",
    };
    QBFsReq *r = fs_req_new(ctx, argc, argv, names[magic]);
    int n = 0, rc;
    if (!r)
        return JS_EXCEPTION;
    if (!(r->path = fs_cpath(ctx, argv[0])))
        return fs_fail(r);
    if (magic <= 2 && fs_int(ctx, &n, argv[1], magic == 0 ? F_OK : 0777))
        return fs_fail(r);
    switch (magic) {
    case 0:
        rc = uv_fs_access(r->loop, &r->req, r->path, n, FS_CB(r));
        break;
    case 1:
        rc = uv_fs_mkdir(r->loop, &r->req, r->path, n, FS_CB(r));
        break;
    case 2:
        rc = uv_fs_chmod(r->loop, &r->req, r->path, n, FS_CB(r));
        break;
    case 3:
        rc = uv_fs_rmdir(r->loop, &r->req, r->path, FS_CB(r));
        break;
    case 4:
        rc = uv_fs_unlink(r->loop, &r->req, r->path, FS_CB(r));
        break;
    case 5:
        rc = uv_fs_readlink(r->loop, &r->req, r->path, FS_CB(r));
        break;
    case 6:
        rc = uv_fs_realpath(r->loop, &r->req, r->path, FS_CB(r));
        break;
    case 7:
        rc = uv_fs_scandir(r->loop, &r->req, r->path, 0, FS_CB(r));
        break;
    case 8:
        rc = uv_fs_opendir(r->loop, &r->req, r->path, FS_CB(r));
        break;
    default: {
        size_t l = strlen(r->path);
        char *tpl = malloc(l + 7);
        if (!tpl) {
            fs_req_free(r);
            return JS_ThrowOutOfMemory(ctx);
        }
        memcpy(tpl, r->path, l);
        memcpy(tpl + l, "XXXXXX", 7);
        free(r->path);
        r->path = tpl;
        rc = uv_fs_mkdtemp(r->loop, &r->req, r->path, FS_CB(r));
        break;
    }
    }
    return fs_finish(ctx, r, rc);
}

/* two-path ops: rename(a, b), link(a, b), copyfile(a, b, flags),
   symlink(target, path, flags) */
static JSValue qb_fs_pathop2(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv,
                             int magic)
{
    static const char *const names[] = {"rename", "link", "copyfile", "symlink"};
    QBFsReq *r = fs_req_new(ctx, argc, argv, names[magic]);
    int flags = 0, rc;
    if (!r)
        return JS_EXCEPTION;
    if (!(r->path = fs_cpath(ctx, argv[0])) || !(r->dest = fs_cpath(ctx, argv[1])))
        return fs_fail(r);
    if (magic >= 2 && fs_int(ctx, &flags, argv[2], 0))
        return fs_fail(r);
    switch (magic) {
    case 0:
        rc = uv_fs_rename(r->loop, &r->req, r->path, r->dest, FS_CB(r));
        break;
    case 1:
        rc = uv_fs_link(r->loop, &r->req, r->path, r->dest, FS_CB(r));
        break;
    case 2:
        rc = uv_fs_copyfile(r->loop, &r->req, r->path, r->dest, flags, FS_CB(r));
        break;
    default:
        rc = uv_fs_symlink(r->loop, &r->req, r->path, r->dest, flags, FS_CB(r));
        break;
    }
    return fs_finish(ctx, r, rc);
}

/* fchmod(fd, mode), ftruncate(fd, len) */
static JSValue qb_fs_fdint(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv,
                           int magic)
{
    QBFsReq *r = fs_req_new(ctx, argc, argv, magic ? "ftruncate" : "fchmod");
    int fd, rc;
    int64_t n;
    if (!r)
        return JS_EXCEPTION;
    if (JS_ToInt32(ctx, &fd, argv[0]) || fs_i64(ctx, &n, argv[1], 0))
        return fs_fail(r);
    if (magic)
        rc = uv_fs_ftruncate(r->loop, &r->req, fd, n < 0 ? 0 : n, FS_CB(r));
    else
        rc = uv_fs_fchmod(r->loop, &r->req, fd, (int)n, FS_CB(r));
    return fs_finish(ctx, r, rc);
}

/* chown/lchown(path, uid, gid), fchown(fd, uid, gid) (magic 0/1/2) */
static JSValue qb_fs_chown(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv,
                           int magic)
{
    static const char *const names[] = {"chown", "lchown", "fchown"};
    QBFsReq *r = fs_req_new(ctx, argc, argv, names[magic]);
    int fd = -1, rc;
    int64_t uid, gid;
    if (!r)
        return JS_EXCEPTION;
    if (magic == 2 ? JS_ToInt32(ctx, &fd, argv[0]) != 0 : !(r->path = fs_cpath(ctx, argv[0])))
        return fs_fail(r);
    if (fs_i64(ctx, &uid, argv[1], -1) || fs_i64(ctx, &gid, argv[2], -1))
        return fs_fail(r);
    switch (magic) {
    case 0:
        rc = uv_fs_chown(r->loop, &r->req, r->path, (uv_uid_t)uid, (uv_gid_t)gid, FS_CB(r));
        break;
    case 1:
        rc = uv_fs_lchown(r->loop, &r->req, r->path, (uv_uid_t)uid, (uv_gid_t)gid, FS_CB(r));
        break;
    default:
        rc = uv_fs_fchown(r->loop, &r->req, fd, (uv_uid_t)uid, (uv_gid_t)gid, FS_CB(r));
        break;
    }
    return fs_finish(ctx, r, rc);
}

/* utimes/lutimes(path, atime, mtime), futimes(fd, atime, mtime) (seconds) */
static JSValue qb_fs_utime(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv,
                           int magic)
{
    static const char *const names[] = {"utime", "lutime", "futime"};
    QBFsReq *r = fs_req_new(ctx, argc, argv, names[magic]);
    int fd = -1, rc;
    double at, mt;
    if (!r)
        return JS_EXCEPTION;
    if (magic == 2 ? JS_ToInt32(ctx, &fd, argv[0]) != 0 : !(r->path = fs_cpath(ctx, argv[0])))
        return fs_fail(r);
    if (JS_ToFloat64(ctx, &at, argv[1]) || JS_ToFloat64(ctx, &mt, argv[2]))
        return fs_fail(r);
    switch (magic) {
    case 0:
        rc = uv_fs_utime(r->loop, &r->req, r->path, at, mt, FS_CB(r));
        break;
    case 1:
        rc = uv_fs_lutime(r->loop, &r->req, r->path, at, mt, FS_CB(r));
        break;
    default:
        rc = uv_fs_futime(r->loop, &r->req, fd, at, mt, FS_CB(r));
        break;
    }
    return fs_finish(ctx, r, rc);
}

/* sendfile(outFd, inFd, inOffset, length, cb?) */
static JSValue qb_fs_sendfile(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    QBFsReq *r = fs_req_new(ctx, argc, argv, "sendfile");
    int out, in;
    int64_t off, len;
    if (!r)
        return JS_EXCEPTION;
    if (JS_ToInt32(ctx, &out, argv[0]) || JS_ToInt32(ctx, &in, argv[1]) ||
        fs_i64(ctx, &off, argv[2], 0) || fs_i64(ctx, &len, argv[3], 0))
        return fs_fail(r);
    return fs_finish(
        ctx, r,
        uv_fs_sendfile(r->loop, &r->req, out, in, off, (size_t)(len < 0 ? 0 : len), FS_CB(r)));
}

/* lchmod(path, mode): synchronous, macOS only (Node exposes it there) */
static JSValue qb_fs_lchmod(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    char *path = fs_cpath(ctx, argv[0]);
    int mode, rc;
    if (!path)
        return JS_EXCEPTION;
    if (JS_ToInt32(ctx, &mode, argv[1])) {
        free(path);
        return JS_EXCEPTION;
    }
#ifdef __APPLE__
    rc = lchmod(path, (mode_t)mode) < 0 ? uv_translate_sys_error(errno) : 0;
#else
    rc = UV_ENOSYS;
#endif
    if (rc < 0) {
        JSValue e = qb_throw_uv(ctx, rc, "lchmod", path, NULL);
        free(path);
        return e;
    }
    free(path);
    return JS_UNDEFINED;
}

/* existsSync(path) → boolean (never throws for a missing file) */
static JSValue qb_fs_exists(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    char *path;
    uv_fs_t req;
    int rc;
    if (!JS_IsString(argv[0]) && !JS_IsObject(argv[0]))
        return JS_FALSE;
    path = fs_cpath(ctx, argv[0]);
    if (!path) {
        JS_FreeValue(ctx, JS_GetException(ctx));
        return JS_FALSE;
    }
    rc = uv_fs_access(qb_get(ctx)->loop, &req, path, F_OK, NULL);
    uv_fs_req_cleanup(&req);
    free(path);
    return JS_NewBool(ctx, rc >= 0);
}

/* ---------------------------------------------------------------- Dir */

static void qb_dir_finalizer(JSRuntime *rt, JSValue val)
{
    QBDir *d = JS_GetOpaque(val, qb_dir_class_id);
    if (!d)
        return;
    if (d->dir) {
        uv_fs_t req;
        uv_fs_closedir(d->loop, &req, d->dir, NULL);
        uv_fs_req_cleanup(&req);
    }
    free(d);
}

static JSClassDef qb_dir_class = {"DirHandle", .finalizer = qb_dir_finalizer};

/* dir.read(n, cb?) → [names, types] | null */
static JSValue qb_dir_read(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    QBDir *d = JS_GetOpaque2(ctx, this_val, qb_dir_class_id);
    QBFsReq *r;
    int n;
    if (!d)
        return JS_EXCEPTION;
    if (!d->dir)
        return qb_throw_code(ctx, JS_PLAIN_ERROR, "ERR_DIR_CLOSED", "Directory handle was closed");
    if (d->busy)
        return qb_throw_code(ctx, JS_PLAIN_ERROR, "ERR_DIR_CONCURRENT_OPERATION",
                             "Cannot do synchronous work on directory handle with concurrent "
                             "asynchronous operations");
    r = fs_req_new(ctx, argc, argv, "scandir");
    if (!r)
        return JS_EXCEPTION;
    if (fs_int(ctx, &n, argv[0], 32))
        return fs_fail(r);
    if (n < 1)
        n = 1;
    r->dirents = calloc((size_t)n, sizeof(uv_dirent_t));
    if (!r->dirents) {
        fs_req_free(r);
        return JS_ThrowOutOfMemory(ctx);
    }
    d->dir->dirents = r->dirents;
    d->dir->nentries = (size_t)n;
    r->keep = JS_DupValue(ctx, this_val);
    if (r->async)
        d->busy = 1;
    return fs_finish(ctx, r, uv_fs_readdir(r->loop, &r->req, d->dir, FS_CB(r)));
}

/* dir.close(cb?) */
static JSValue qb_dir_close(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    QBDir *d = JS_GetOpaque2(ctx, this_val, qb_dir_class_id);
    QBFsReq *r;
    if (!d)
        return JS_EXCEPTION;
    if (!d->dir)
        return qb_throw_code(ctx, JS_PLAIN_ERROR, "ERR_DIR_CLOSED", "Directory handle was closed");
    if (d->busy)
        return qb_throw_code(ctx, JS_PLAIN_ERROR, "ERR_DIR_CONCURRENT_OPERATION",
                             "Cannot do synchronous work on directory handle with concurrent "
                             "asynchronous operations");
    r = fs_req_new(ctx, argc, argv, "closedir");
    if (!r)
        return JS_EXCEPTION;
    r->keep = JS_DupValue(ctx, this_val);
    if (r->async)
        d->busy = 1;
    return fs_finish(ctx, r, uv_fs_closedir(r->loop, &r->req, d->dir, FS_CB(r)));
}

static const JSCFunctionListEntry qb_dir_proto_funcs[] = {
    JS_CFUNC_DEF("read", 2, qb_dir_read),
    JS_CFUNC_DEF("close", 1, qb_dir_close),
};

/* ------------------------------------------------ readFile / writeFile */

typedef struct QBFileWork {
    uv_work_t w;
    JSContext *ctx;
    JSValue cb;
    JSValue keep;
    char *path; /* NULL: use fd */
    int fd;
    int flags;
    int mode;
    uint8_t *data; /* readFile: malloc'd result; writeFile: borrowed */
    size_t len;
    int err;
    const char *syscall;
} QBFileWork;

static void fs_free_malloc(JSRuntime *rt, void *opaque, void *ptr)
{
    free(ptr);
}

/* blocking whole-file read (runs on a pool thread for the async form) */
static void fs_do_read_file(QBFileWork *fw)
{
    int fd = fw->fd, own = 0;
    struct stat st;
    size_t cap, len = 0;
    uint8_t *buf;
    if (fw->path) {
        do
            fd = open(fw->path, fw->flags | O_CLOEXEC);
        while (fd < 0 && errno == EINTR);
        if (fd < 0) {
            fw->err = uv_translate_sys_error(errno);
            fw->syscall = "open";
            return;
        }
        own = 1;
    }
    if (fstat(fd, &st) < 0) {
        fw->err = uv_translate_sys_error(errno);
        fw->syscall = "fstat";
        goto done;
    }
    if (S_ISDIR(st.st_mode)) {
        fw->err = UV_EISDIR;
        fw->syscall = "read";
        goto done;
    }
    cap = S_ISREG(st.st_mode) && st.st_size > 0 ? (size_t)st.st_size + 1 : 16384;
    buf = malloc(cap);
    if (!buf) {
        fw->err = UV_ENOMEM;
        fw->syscall = "read";
        goto done;
    }
    for (;;) {
        ssize_t n;
        if (len == cap) {
            size_t ncap = cap * 2;
            uint8_t *nb = realloc(buf, ncap);
            if (!nb) {
                free(buf);
                fw->err = UV_ENOMEM;
                fw->syscall = "read";
                goto done;
            }
            buf = nb;
            cap = ncap;
        }
        n = read(fd, buf + len, cap - len);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            free(buf);
            fw->err = uv_translate_sys_error(errno);
            fw->syscall = "read";
            goto done;
        }
        if (n == 0)
            break;
        len += (size_t)n;
    }
    fw->data = buf;
    fw->len = len;
done:
    if (own)
        close(fd);
}

static void fs_do_write_file(QBFileWork *fw)
{
    int fd = fw->fd, own = 0;
    size_t off = 0;
    if (fw->path) {
        do
            fd = open(fw->path, fw->flags | O_CLOEXEC, fw->mode);
        while (fd < 0 && errno == EINTR);
        if (fd < 0) {
            fw->err = uv_translate_sys_error(errno);
            fw->syscall = "open";
            return;
        }
        own = 1;
    }
    while (off < fw->len) {
        ssize_t n = write(fd, fw->data + off, fw->len - off);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            fw->err = uv_translate_sys_error(errno);
            fw->syscall = "write";
            break;
        }
        off += (size_t)n;
    }
    if (own && close(fd) < 0 && !fw->err) {
        fw->err = uv_translate_sys_error(errno);
        fw->syscall = "close";
    }
}

static void fs_read_file_work(uv_work_t *w)
{
    fs_do_read_file(w->data);
}

static void fs_write_file_work(uv_work_t *w)
{
    fs_do_write_file(w->data);
}

static JSValue fs_file_result(JSContext *ctx, QBFileWork *fw, int is_read, int *is_err)
{
    *is_err = 0;
    if (fw->err) {
        *is_err = 1;
        return qb_uv_error(ctx, fw->err, fw->syscall, fw->path, NULL);
    }
    if (!is_read)
        return JS_UNDEFINED;
    {
        uint8_t *d = fw->data;
        fw->data = NULL;
        if (!d)
            return qb_new_buffer(ctx, (const uint8_t *)"", 0);
        return qb_to_buffer(ctx, JS_NewUint8Array(ctx, d, fw->len, fs_free_malloc, NULL, 0));
    }
}

static void fs_file_work_free(QBFileWork *fw, int is_read)
{
    JS_FreeValue(fw->ctx, fw->cb);
    JS_FreeValue(fw->ctx, fw->keep);
    if (is_read)
        free(fw->data);
    free(fw->path);
    free(fw);
}

static void fs_file_after(uv_work_t *w, int status, int is_read)
{
    QBFileWork *fw = w->data;
    JSContext *ctx = fw->ctx;
    JSValue args[2], cb = fw->cb, ret, res;
    int is_err;
    if (status < 0 && !fw->err) {
        fw->err = status;
        fw->syscall = is_read ? "read" : "write";
    }
    res = fs_file_result(ctx, fw, is_read, &is_err);
    if (JS_IsException(res)) {
        args[0] = JS_GetException(ctx);
        args[1] = JS_UNDEFINED;
    } else if (is_err) {
        args[0] = res;
        args[1] = JS_UNDEFINED;
    } else {
        args[0] = JS_NULL;
        args[1] = res;
    }
    fw->cb = JS_UNDEFINED;
    fs_file_work_free(fw, is_read);
    ret = qb_call(ctx, cb, JS_UNDEFINED, 2, (JSValueConst *)args);
    JS_FreeValue(ctx, ret);
    JS_FreeValue(ctx, cb);
    JS_FreeValue(ctx, args[0]);
    JS_FreeValue(ctx, args[1]);
}

static void fs_read_file_after(uv_work_t *w, int status)
{
    fs_file_after(w, status, 1);
}

static void fs_write_file_after(uv_work_t *w, int status)
{
    fs_file_after(w, status, 0);
}

static QBFileWork *fs_file_work_new(JSContext *ctx, int argc, JSValueConst *argv)
{
    QBFileWork *fw = calloc(1, sizeof(*fw));
    if (!fw) {
        JS_ThrowOutOfMemory(ctx);
        return NULL;
    }
    fw->ctx = ctx;
    fw->cb = JS_UNDEFINED;
    fw->keep = JS_UNDEFINED;
    fw->fd = -1;
    fw->w.data = fw;
    if (argc > 0 && JS_IsFunction(ctx, argv[argc - 1]))
        fw->cb = JS_DupValue(ctx, argv[argc - 1]);
    if (JS_IsNumber(argv[0])) {
        if (JS_ToInt32(ctx, &fw->fd, argv[0]))
            goto fail;
    } else if (!(fw->path = fs_cpath(ctx, argv[0]))) {
        goto fail;
    }
    return fw;
fail:
    JS_FreeValue(ctx, fw->cb);
    free(fw);
    return NULL;
}

/* readFile(pathOrFd, flags, cb?) → Buffer */
static JSValue qb_fs_readFile(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    QBFileWork *fw = fs_file_work_new(ctx, argc, argv);
    int is_err, rc;
    JSValue res;
    if (!fw)
        return JS_EXCEPTION;
    if (fs_int(ctx, &fw->flags, argv[1], O_RDONLY)) {
        fs_file_work_free(fw, 1);
        return JS_EXCEPTION;
    }
    if (!JS_IsUndefined(fw->cb)) {
        rc = uv_queue_work(qb_get(ctx)->loop, &fw->w, fs_read_file_work, fs_read_file_after);
        if (rc < 0) {
            res = qb_throw_uv(ctx, rc, "read", fw->path, NULL);
            fs_file_work_free(fw, 1);
            return res;
        }
        return JS_UNDEFINED;
    }
    fs_do_read_file(fw);
    res = fs_file_result(ctx, fw, 1, &is_err);
    fs_file_work_free(fw, 1);
    return is_err ? JS_Throw(ctx, res) : res;
}

/* writeFile(pathOrFd, bytes, flags, mode, cb?) */
static JSValue qb_fs_writeFile(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    QBFileWork *fw = fs_file_work_new(ctx, argc, argv);
    int is_err, rc;
    JSValue res;
    if (!fw)
        return JS_EXCEPTION;
    fw->data = qb_get_bytes(ctx, argv[1], &fw->len);
    if ((!fw->data && JS_HasException(ctx)) ||
        fs_int(ctx, &fw->flags, argv[2], O_WRONLY | O_CREAT | O_TRUNC) ||
        fs_int(ctx, &fw->mode, argv[3], 0666)) {
        fw->data = NULL;
        fs_file_work_free(fw, 0);
        return JS_EXCEPTION;
    }
    if (!fw->data)
        fw->len = 0;
    if (!JS_IsUndefined(fw->cb)) {
        fw->keep = JS_DupValue(ctx, argv[1]);
        rc = uv_queue_work(qb_get(ctx)->loop, &fw->w, fs_write_file_work, fs_write_file_after);
        if (rc < 0) {
            res = qb_throw_uv(ctx, rc, "write", fw->path, NULL);
            fs_file_work_free(fw, 0);
            return res;
        }
        return JS_UNDEFINED;
    }
    fs_do_write_file(fw);
    res = fs_file_result(ctx, fw, 0, &is_err);
    fs_file_work_free(fw, 0);
    return is_err ? JS_Throw(ctx, res) : res;
}

/* ------------------------------------------------------------ watchers */

typedef struct QBFsWatch {
    union {
        uv_handle_t handle;
        uv_fs_event_t event;
        uv_fs_poll_t poll;
    } h;
    QBRuntime *qr;
    int is_poll;
    int bigint;
    JSValue cb;
    JSValue self; /* strong while started */
    int closed;
    int close_done;
    int js_dead;
} QBFsWatch;

static void qb_fswatch_close_cb(uv_handle_t *h)
{
    QBFsWatch *w = h->data;
    w->close_done = 1;
    if (w->js_dead)
        free(w);
}

static void qb_fswatch_finalizer(JSRuntime *rt, JSValue val)
{
    QBFsWatch *w = JS_GetOpaque(val, qb_fswatch_class_id);
    if (!w)
        return;
    JS_FreeValueRT(rt, w->cb);
    w->cb = JS_UNDEFINED;
    w->js_dead = 1;
    if (!w->closed) {
        w->closed = 1;
        uv_close(&w->h.handle, qb_fswatch_close_cb);
    } else if (w->close_done) {
        free(w);
    }
}

static void qb_fswatch_mark(JSRuntime *rt, JSValueConst val, JS_MarkFunc *mark_func)
{
    QBFsWatch *w = JS_GetOpaque(val, qb_fswatch_class_id);
    if (w)
        JS_MarkValue(rt, w->cb, mark_func);
}

static JSClassDef qb_fswatch_class = {
    "FSWatcherHandle",
    .finalizer = qb_fswatch_finalizer,
    .gc_mark = qb_fswatch_mark,
};

static void qb_fswatch_release(QBFsWatch *w)
{
    JSValue self = w->self;
    w->self = JS_UNDEFINED;
    JS_FreeValue(w->qr->ctx, self);
}

static void qb_fsevent_cb(uv_fs_event_t *h, const char *filename, int events, int status)
{
    QBFsWatch *w = h->data;
    JSContext *ctx = w->qr->ctx;
    JSValue args[3], cb, self, ret;
    if (w->closed)
        return;
    if (status < 0) {
        char path[4096];
        size_t sz = sizeof(path);
        if (uv_fs_event_getpath(h, path, &sz) < 0)
            path[0] = '\0';
        args[0] = qb_uv_error(ctx, status, "watch", path, NULL);
        args[1] = JS_UNDEFINED;
        args[2] = JS_UNDEFINED;
    } else {
        args[0] = JS_NULL;
        args[1] = JS_NewString(ctx, (events & UV_RENAME) ? "rename" : "change");
        args[2] = filename ? JS_NewString(ctx, filename) : JS_NULL;
    }
    cb = JS_DupValue(ctx, w->cb);
    self = JS_DupValue(ctx, w->self);
    ret = qb_call(ctx, cb, self, 3, (JSValueConst *)args);
    JS_FreeValue(ctx, ret);
    JS_FreeValue(ctx, cb);
    JS_FreeValue(ctx, self);
    JS_FreeValue(ctx, args[0]);
    JS_FreeValue(ctx, args[1]);
    JS_FreeValue(ctx, args[2]);
}

static void qb_fspoll_cb(uv_fs_poll_t *h, int status, const uv_stat_t *prev, const uv_stat_t *curr)
{
    QBFsWatch *w = h->data;
    JSContext *ctx = w->qr->ctx;
    JSValue args[3], cb, self, ret;
    if (w->closed)
        return;
    args[0] = status < 0 ? JS_NewInt32(ctx, status) : JS_NULL;
    args[1] = fs_stats(ctx, curr, w->bigint);
    args[2] = fs_stats(ctx, prev, w->bigint);
    cb = JS_DupValue(ctx, w->cb);
    self = JS_DupValue(ctx, w->self);
    ret = qb_call(ctx, cb, self, 3, (JSValueConst *)args);
    JS_FreeValue(ctx, ret);
    JS_FreeValue(ctx, cb);
    JS_FreeValue(ctx, self);
    JS_FreeValue(ctx, args[1]);
    JS_FreeValue(ctx, args[2]);
}

/* createWatcher(cb(err, eventType, filename)) / createStatWatcher(cb(status|null,
   curr, prev), bigint) (magic 1) */
static JSValue qb_fs_createWatcher(JSContext *ctx, JSValueConst this_val, int argc,
                                   JSValueConst *argv, int magic)
{
    QBRuntime *qr = qb_get(ctx);
    QBFsWatch *w;
    JSValue obj;
    if (!JS_IsFunction(ctx, argv[0]))
        return JS_ThrowTypeError(ctx, "callback must be a function");
    obj = JS_NewObjectClass(ctx, qb_fswatch_class_id);
    if (JS_IsException(obj))
        return obj;
    w = calloc(1, sizeof(*w));
    if (!w) {
        JS_FreeValue(ctx, obj);
        return JS_ThrowOutOfMemory(ctx);
    }
    w->qr = qr;
    w->is_poll = magic;
    w->bigint = magic && argc > 1 && JS_ToBool(ctx, argv[1]) > 0;
    w->cb = JS_DupValue(ctx, argv[0]);
    w->self = JS_UNDEFINED;
    if (magic)
        uv_fs_poll_init(qr->loop, &w->h.poll);
    else
        uv_fs_event_init(qr->loop, &w->h.event);
    w->h.handle.data = w;
    JS_SetOpaque(obj, w);
    return obj;
}

/* handle.start(path, recursive | intervalMs) */
static JSValue qb_fswatch_start(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    QBFsWatch *w = JS_GetOpaque2(ctx, this_val, qb_fswatch_class_id);
    char *path;
    int rc;
    if (!w)
        return JS_EXCEPTION;
    if (w->closed)
        return JS_UNDEFINED;
    path = fs_cpath(ctx, argv[0]);
    if (!path)
        return JS_EXCEPTION;
    if (w->is_poll) {
        double iv = 5007;
        if (argc > 1 && !JS_IsUndefined(argv[1]) && JS_ToFloat64(ctx, &iv, argv[1])) {
            free(path);
            return JS_EXCEPTION;
        }
        if (!(iv >= 1))
            iv = 1;
        rc = uv_fs_poll_start(&w->h.poll, qb_fspoll_cb, path, (unsigned int)iv);
    } else {
        unsigned int flags = (argc > 1 && JS_ToBool(ctx, argv[1]) > 0) ? UV_FS_EVENT_RECURSIVE : 0;
        rc = uv_fs_event_start(&w->h.event, qb_fsevent_cb, path, flags);
    }
    if (rc < 0) {
        JSValue e = qb_throw_uv(ctx, rc, "watch", path, NULL);
        free(path);
        return e;
    }
    free(path);
    if (JS_IsUndefined(w->self))
        w->self = JS_DupValue(ctx, this_val);
    return JS_UNDEFINED;
}

static JSValue qb_fswatch_method(JSContext *ctx, JSValueConst this_val, int argc,
                                 JSValueConst *argv, int magic)
{
    QBFsWatch *w = JS_GetOpaque2(ctx, this_val, qb_fswatch_class_id);
    if (!w)
        return JS_EXCEPTION;
    switch (magic) {
    case 0: /* close */
        if (!w->closed) {
            if (w->is_poll)
                uv_fs_poll_stop(&w->h.poll);
            else
                uv_fs_event_stop(&w->h.event);
            w->closed = 1;
            uv_close(&w->h.handle, qb_fswatch_close_cb);
        }
        qb_fswatch_release(w);
        break;
    case 1:
        if (!w->closed)
            uv_ref(&w->h.handle);
        break;
    case 2:
        if (!w->closed)
            uv_unref(&w->h.handle);
        break;
    case 3:
        return JS_NewBool(ctx, !w->closed && uv_has_ref(&w->h.handle));
    case 4: { /* getPath */
        char path[4096];
        size_t sz = sizeof(path);
        int rc;
        if (w->closed)
            return JS_UNDEFINED;
        rc = w->is_poll ? uv_fs_poll_getpath(&w->h.poll, path, &sz)
                        : uv_fs_event_getpath(&w->h.event, path, &sz);
        if (rc < 0)
            return JS_UNDEFINED;
        return JS_NewStringLen(ctx, path, sz);
    }
    }
    return JS_UNDEFINED;
}

static const JSCFunctionListEntry qb_fswatch_proto_funcs[] = {
    JS_CFUNC_DEF("start", 2, qb_fswatch_start),
    JS_CFUNC_MAGIC_DEF("close", 0, qb_fswatch_method, 0),
    JS_CFUNC_MAGIC_DEF("ref", 0, qb_fswatch_method, 1),
    JS_CFUNC_MAGIC_DEF("unref", 0, qb_fswatch_method, 2),
    JS_CFUNC_MAGIC_DEF("hasRef", 0, qb_fswatch_method, 3),
    JS_CFUNC_MAGIC_DEF("getPath", 0, qb_fswatch_method, 4),
};

/* ------------------------------------------------------------ constants */

#define C(name) {#name, name}
static const struct {
    const char *name;
    int64_t value;
} qb_fs_constants[] = {
    {"UV_FS_SYMLINK_DIR", UV_FS_SYMLINK_DIR},
    {"UV_FS_SYMLINK_JUNCTION", UV_FS_SYMLINK_JUNCTION},
    C(O_RDONLY),
    C(O_WRONLY),
    C(O_RDWR),
    {"UV_DIRENT_UNKNOWN", UV_DIRENT_UNKNOWN},
    {"UV_DIRENT_FILE", UV_DIRENT_FILE},
    {"UV_DIRENT_DIR", UV_DIRENT_DIR},
    {"UV_DIRENT_LINK", UV_DIRENT_LINK},
    {"UV_DIRENT_FIFO", UV_DIRENT_FIFO},
    {"UV_DIRENT_SOCKET", UV_DIRENT_SOCKET},
    {"UV_DIRENT_CHAR", UV_DIRENT_CHAR},
    {"UV_DIRENT_BLOCK", UV_DIRENT_BLOCK},
    C(S_IFMT),
    C(S_IFREG),
    C(S_IFDIR),
    C(S_IFCHR),
    C(S_IFBLK),
    C(S_IFIFO),
    C(S_IFLNK),
    C(S_IFSOCK),
    C(O_CREAT),
    C(O_EXCL),
    {"UV_FS_O_FILEMAP", 0},
    C(O_NOCTTY),
    C(O_TRUNC),
    C(O_APPEND),
    C(O_DIRECTORY),
#ifdef O_NOATIME
    C(O_NOATIME),
#endif
    C(O_NOFOLLOW),
    C(O_SYNC),
#ifdef O_DSYNC
    C(O_DSYNC),
#endif
#ifdef O_SYMLINK
    C(O_SYMLINK),
#endif
#ifdef O_DIRECT
    C(O_DIRECT),
#endif
    C(O_NONBLOCK),
    C(S_IRWXU),
    C(S_IRUSR),
    C(S_IWUSR),
    C(S_IXUSR),
    C(S_IRWXG),
    C(S_IRGRP),
    C(S_IWGRP),
    C(S_IXGRP),
    C(S_IRWXO),
    C(S_IROTH),
    C(S_IWOTH),
    C(S_IXOTH),
    C(F_OK),
    C(R_OK),
    C(W_OK),
    C(X_OK),
    {"UV_FS_COPYFILE_EXCL", UV_FS_COPYFILE_EXCL},
    {"COPYFILE_EXCL", UV_FS_COPYFILE_EXCL},
    {"UV_FS_COPYFILE_FICLONE", UV_FS_COPYFILE_FICLONE},
    {"COPYFILE_FICLONE", UV_FS_COPYFILE_FICLONE},
    {"UV_FS_COPYFILE_FICLONE_FORCE", UV_FS_COPYFILE_FICLONE_FORCE},
    {"COPYFILE_FICLONE_FORCE", UV_FS_COPYFILE_FICLONE_FORCE},
};
#undef C

static JSValue qb_fs_make_constants(JSContext *ctx)
{
    JSValue obj = JS_NewObject(ctx);
    size_t i;
    if (JS_IsException(obj))
        return obj;
    for (i = 0; i < countof(qb_fs_constants); i++) {
        if (qb_set(ctx, obj, qb_fs_constants[i].name, JS_NewInt64(ctx, qb_fs_constants[i].value)) <
            0) {
            JS_FreeValue(ctx, obj);
            return JS_EXCEPTION;
        }
    }
    return obj;
}

/* ------------------------------------------------------------ module */

static const JSCFunctionListEntry qb_fs_funcs[] = {
    JS_CFUNC_DEF("open", 4, qb_fs_open),
    JS_CFUNC_MAGIC_DEF("close", 2, qb_fs_fdop, 0),
    JS_CFUNC_MAGIC_DEF("fsync", 2, qb_fs_fdop, 1),
    JS_CFUNC_MAGIC_DEF("fdatasync", 2, qb_fs_fdop, 2),
    JS_CFUNC_MAGIC_DEF("read", 6, qb_fs_rw, 0),
    JS_CFUNC_MAGIC_DEF("write", 6, qb_fs_rw, 1),
    JS_CFUNC_MAGIC_DEF("readv", 4, qb_fs_rwv, 0),
    JS_CFUNC_MAGIC_DEF("writev", 4, qb_fs_rwv, 1),
    JS_CFUNC_MAGIC_DEF("stat", 3, qb_fs_stat, 0),
    JS_CFUNC_MAGIC_DEF("lstat", 3, qb_fs_stat, 1),
    JS_CFUNC_MAGIC_DEF("statfs", 3, qb_fs_stat, 2),
    JS_CFUNC_DEF("fstat", 3, qb_fs_fstat),
    JS_CFUNC_MAGIC_DEF("access", 3, qb_fs_pathop, 0),
    JS_CFUNC_MAGIC_DEF("mkdir", 3, qb_fs_pathop, 1),
    JS_CFUNC_MAGIC_DEF("chmod", 3, qb_fs_pathop, 2),
    JS_CFUNC_MAGIC_DEF("rmdir", 2, qb_fs_pathop, 3),
    JS_CFUNC_MAGIC_DEF("unlink", 2, qb_fs_pathop, 4),
    JS_CFUNC_MAGIC_DEF("readlink", 2, qb_fs_pathop, 5),
    JS_CFUNC_MAGIC_DEF("realpath", 2, qb_fs_pathop, 6),
    JS_CFUNC_MAGIC_DEF("scandir", 2, qb_fs_pathop, 7),
    JS_CFUNC_MAGIC_DEF("opendir", 2, qb_fs_pathop, 8),
    JS_CFUNC_MAGIC_DEF("mkdtemp", 2, qb_fs_pathop, 9),
    JS_CFUNC_MAGIC_DEF("rename", 3, qb_fs_pathop2, 0),
    JS_CFUNC_MAGIC_DEF("link", 3, qb_fs_pathop2, 1),
    JS_CFUNC_MAGIC_DEF("copyfile", 4, qb_fs_pathop2, 2),
    JS_CFUNC_MAGIC_DEF("symlink", 4, qb_fs_pathop2, 3),
    JS_CFUNC_MAGIC_DEF("fchmod", 3, qb_fs_fdint, 0),
    JS_CFUNC_MAGIC_DEF("ftruncate", 3, qb_fs_fdint, 1),
    JS_CFUNC_MAGIC_DEF("chown", 4, qb_fs_chown, 0),
    JS_CFUNC_MAGIC_DEF("lchown", 4, qb_fs_chown, 1),
    JS_CFUNC_MAGIC_DEF("fchown", 4, qb_fs_chown, 2),
    JS_CFUNC_MAGIC_DEF("utimes", 4, qb_fs_utime, 0),
    JS_CFUNC_MAGIC_DEF("lutimes", 4, qb_fs_utime, 1),
    JS_CFUNC_MAGIC_DEF("futimes", 4, qb_fs_utime, 2),
    JS_CFUNC_DEF("sendfile", 5, qb_fs_sendfile),
    JS_CFUNC_DEF("lchmod", 2, qb_fs_lchmod),
    JS_CFUNC_DEF("existsSync", 1, qb_fs_exists),
    JS_CFUNC_DEF("readFile", 3, qb_fs_readFile),
    JS_CFUNC_DEF("writeFile", 5, qb_fs_writeFile),
    JS_CFUNC_MAGIC_DEF("createWatcher", 1, qb_fs_createWatcher, 0),
    JS_CFUNC_MAGIC_DEF("createStatWatcher", 2, qb_fs_createWatcher, 1),
};

static int qb_fs_module_init(JSContext *ctx, JSModuleDef *m)
{
    JSRuntime *rt = JS_GetRuntime(ctx);
    JSValue proto;
    JS_NewClassID(&qb_dir_class_id);
    JS_NewClassID(&qb_fswatch_class_id);
    if (!JS_IsRegisteredClass(rt, qb_dir_class_id)) {
        JS_NewClass(rt, qb_dir_class_id, &qb_dir_class);
        proto = JS_NewObject(ctx);
        JS_SetPropertyFunctionList(ctx, proto, qb_dir_proto_funcs, countof(qb_dir_proto_funcs));
        JS_SetClassProto(ctx, qb_dir_class_id, proto);
    }
    if (!JS_IsRegisteredClass(rt, qb_fswatch_class_id)) {
        JS_NewClass(rt, qb_fswatch_class_id, &qb_fswatch_class);
        proto = JS_NewObject(ctx);
        JS_SetPropertyFunctionList(ctx, proto, qb_fswatch_proto_funcs,
                                   countof(qb_fswatch_proto_funcs));
        JS_SetClassProto(ctx, qb_fswatch_class_id, proto);
    }
    if (JS_SetModuleExportList(ctx, m, qb_fs_funcs, countof(qb_fs_funcs)) < 0)
        return -1;
    return JS_SetModuleExport(ctx, m, "constants", qb_fs_make_constants(ctx));
}

JSModuleDef *qb_init_fs(JSContext *ctx, const char *name)
{
    JSModuleDef *m = JS_NewCModule(ctx, name, qb_fs_module_init);
    if (!m)
        return NULL;
    if (JS_AddModuleExportList(ctx, m, qb_fs_funcs, countof(qb_fs_funcs)) < 0 ||
        JS_AddModuleExport(ctx, m, "constants") < 0)
        return NULL;
    return m;
}
