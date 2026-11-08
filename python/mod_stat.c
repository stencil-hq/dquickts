/*
 * _stat module (port of CPython Modules/_stat.c, PSF-2.0): S_IF* / S_I*
 * constants, S_IS*() predicates, S_IMODE/S_IFMT and filemode().
 */
#include <sys/stat.h>

#ifndef S_IMODE
#  define S_IMODE 07777
#endif

/* S_IFXXX constants (file types)
 *
 * Only the names are defined by POSIX but not their value. All common file
 * types seems to have the same numeric value on all platforms, though.
 *
 * fileutils.h guarantees S_IFMT, S_IFDIR, S_IFCHR, S_IFREG and S_IFLNK
 */

#ifndef S_IFBLK
#  define S_IFBLK 0060000
#endif

#ifndef S_IFIFO
#  define S_IFIFO 0010000
#endif

#ifndef S_IFSOCK
#  define S_IFSOCK 0140000
#endif

#ifndef S_IFDOOR
#  define S_IFDOOR 0
#endif

#ifndef S_IFPORT
#  define S_IFPORT 0
#endif

#ifndef S_IFWHT
#  define S_IFWHT 0
#endif


/* S_ISXXX()
 * fileutils.h defines S_ISDIR(), S_ISREG() and S_ISCHR()
 */

#ifndef S_ISBLK
#  define S_ISBLK(mode) (((mode) & S_IFMT) == S_IFBLK)
#endif

#ifndef S_ISFIFO
#  define S_ISFIFO(mode) (((mode) & S_IFMT) == S_IFIFO)
#endif

#ifndef S_ISLNK
#  define S_ISLNK(mode) (((mode) & S_IFMT) == S_IFLNK)
#endif

#ifndef S_ISSOCK
#  define S_ISSOCK(mode) (((mode) & S_IFMT) == S_IFSOCK)
#endif

#ifndef S_ISDOOR
#  define S_ISDOOR(mode) 0
#endif

#ifndef S_ISPORT
#  define S_ISPORT(mode) 0
#endif

#ifndef S_ISWHT
#  define S_ISWHT(mode) 0
#endif


/* S_I* file permission
 *
 * The permission bit value are defined by POSIX standards.
 */
#ifndef S_ISUID
#  define S_ISUID 04000
#endif

#ifndef S_ISGID
#  define S_ISGID 02000
#endif

/* what is S_ENFMT? */
#ifndef S_ENFMT
#  define S_ENFMT S_ISGID
#endif

#ifndef S_ISVTX
#  define S_ISVTX 01000
#endif

#ifndef S_IREAD
#  define S_IREAD 00400
#endif

#ifndef S_IWRITE
#  define S_IWRITE 00200
#endif

#ifndef S_IEXEC
#  define S_IEXEC 00100
#endif

#ifndef S_IRWXU
#  define S_IRWXU 00700
#endif

#ifndef S_IRUSR
#  define S_IRUSR 00400
#endif

#ifndef S_IWUSR
#  define S_IWUSR 00200
#endif

#ifndef S_IXUSR
#  define S_IXUSR 00100
#endif

#ifndef S_IRWXG
#  define S_IRWXG 00070
#endif

#ifndef S_IRGRP
#  define S_IRGRP 00040
#endif

#ifndef S_IWGRP
#  define S_IWGRP 00020
#endif

#ifndef S_IXGRP
#  define S_IXGRP 00010
#endif

#ifndef S_IRWXO
#  define S_IRWXO 00007
#endif

#ifndef S_IROTH
#  define S_IROTH 00004
#endif

#ifndef S_IWOTH
#  define S_IWOTH 00002
#endif

#ifndef S_IXOTH
#  define S_IXOTH 00001
#endif


/* Names for file flags */
#ifndef UF_SETTABLE
#  define UF_SETTABLE 0x0000ffff
#endif

#ifndef UF_NODUMP
#  define UF_NODUMP 0x00000001
#endif

#ifndef UF_IMMUTABLE
#  define UF_IMMUTABLE 0x00000002
#endif

#ifndef UF_APPEND
#  define UF_APPEND 0x00000004
#endif

#ifndef UF_OPAQUE
#  define UF_OPAQUE 0x00000008
#endif

#ifndef UF_NOUNLINK
#  define UF_NOUNLINK 0x00000010
#endif

#ifndef UF_COMPRESSED
#  define UF_COMPRESSED 0x00000020
#endif

#ifndef UF_TRACKED
#  define UF_TRACKED 0x00000040
#endif

#ifndef UF_DATAVAULT
#  define UF_DATAVAULT 0x00000080
#endif

#ifndef UF_HIDDEN
#  define UF_HIDDEN 0x00008000
#endif

#ifndef SF_SETTABLE
#  define SF_SETTABLE 0xffff0000
#endif

#ifndef SF_ARCHIVED
#  define SF_ARCHIVED 0x00010000
#endif

#ifndef SF_IMMUTABLE
#  define SF_IMMUTABLE 0x00020000
#endif

#ifndef SF_APPEND
#  define SF_APPEND 0x00040000
#endif

#ifndef SF_NOUNLINK
#  define SF_NOUNLINK 0x00100000
#endif

#ifndef SF_SNAPSHOT
#  define SF_SNAPSHOT 0x00200000
#endif

#ifndef SF_FIRMLINK
#  define SF_FIRMLINK 0x00800000
#endif

#ifndef SF_DATALESS
#  define SF_DATALESS 0x40000000
#endif

#if defined(__APPLE__) && !defined(SF_SUPPORTED)
   /* On older macOS versions the definition of SF_SUPPORTED is different
    * from that on newer versions.
    *
    * Provide a consistent experience by redefining.
    *
    * None of bit bits set in the actual SF_SUPPORTED but not in this
    * definition are defined on these versions of macOS.
    */
#  undef SF_SETTABLE
#  define SF_SUPPORTED 0x009f0000
#  define SF_SETTABLE 0x3fff0000
#  define SF_SYNTHETIC 0xc0000000
#endif



/* _PyLong_AsMode_t; -1 with exception on error */
static int qp_stat_as_mode(JSContext *ctx, JSValueConst op, mode_t *out)
{
    JSValue v = qp_index(ctx, op);
    int64_t value;
    mode_t mode;
    if (JS_IsException(v))
        return -1;
    if (qp_long_sign(v) < 0) {
        JS_FreeValue(ctx, v);
        qp_raise_overflow_error(ctx, "can't convert negative value to unsigned int");
        return -1;
    }
    if (qp_long_as_int64(ctx, v, &value) < 0) {
        JS_FreeValue(ctx, v);
        if (qp_exc_pending_matches(ctx, QP_EXC(ctx, OverflowError))) {
            JS_FreeValue(ctx, JS_GetException(ctx));
            qp_raise_overflow_error(ctx, "Python int too large to convert to C unsigned long");
        }
        return -1;
    }
    JS_FreeValue(ctx, v);
    mode = (mode_t)value;
    if ((int64_t)mode != value) {
        qp_raise_overflow_error(ctx, "mode out of range");
        return -1;
    }
    *out = mode;
    return 0;
}

enum {
    QP_STAT_S_ISDIR,
    QP_STAT_S_ISCHR,
    QP_STAT_S_ISBLK,
    QP_STAT_S_ISREG,
    QP_STAT_S_ISFIFO,
    QP_STAT_S_ISLNK,
    QP_STAT_S_ISSOCK,
    QP_STAT_S_ISDOOR,
    QP_STAT_S_ISPORT,
    QP_STAT_S_ISWHT,
};

static JSValue qp_stat_isfunc(JSContext *ctx, JSValueConst self, JSValueConst *args,
                              int nargs, JSValueConst kwnames, int magic)
{
    mode_t mode;
    int r = 0;
    if (qp_stat_as_mode(ctx, args[0], &mode) < 0)
        return JS_EXCEPTION;
    switch (magic) {
    case QP_STAT_S_ISDIR: r = S_ISDIR(mode) != 0; break;
    case QP_STAT_S_ISCHR: r = S_ISCHR(mode) != 0; break;
    case QP_STAT_S_ISBLK: r = S_ISBLK(mode) != 0; break;
    case QP_STAT_S_ISREG: r = S_ISREG(mode) != 0; break;
    case QP_STAT_S_ISFIFO: r = S_ISFIFO(mode) != 0; break;
    case QP_STAT_S_ISLNK: r = S_ISLNK(mode) != 0; break;
    case QP_STAT_S_ISSOCK: r = S_ISSOCK(mode) != 0; break;
    case QP_STAT_S_ISDOOR: r = S_ISDOOR(mode) != 0; break;
    case QP_STAT_S_ISPORT: r = S_ISPORT(mode) != 0; break;
    case QP_STAT_S_ISWHT: r = S_ISWHT(mode) != 0; break;
    }
    return qp_bool_new(r);
}

static JSValue qp_stat_S_IMODE(JSContext *ctx, JSValueConst self, JSValueConst *args,
                               int nargs, JSValueConst kwnames, int magic)
{
    mode_t mode;
    if (qp_stat_as_mode(ctx, args[0], &mode) < 0)
        return JS_EXCEPTION;
    return qp_long_from_uint64(ctx, (uint64_t)(mode & (magic ? S_IFMT : S_IMODE)));
}

static char qp_stat_filetype(mode_t mode)
{
    /* common cases first */
    if (S_ISREG(mode))  return '-';
    if (S_ISDIR(mode))  return 'd';
    if (S_ISLNK(mode))  return 'l';
    /* special files */
    if (S_ISBLK(mode))  return 'b';
    if (S_ISCHR(mode))  return 'c';
    if (S_ISFIFO(mode)) return 'p';
    if (S_ISSOCK(mode)) return 's';
    /* non-standard types */
    if (S_ISDOOR(mode)) return 'D';
    if (S_ISPORT(mode)) return 'P';
    if (S_ISWHT(mode))  return 'w';
    /* unknown */
    return '?';
}

static void qp_stat_fileperm(mode_t mode, char *buf)
{
    buf[0] = mode & S_IRUSR ? 'r' : '-';
    buf[1] = mode & S_IWUSR ? 'w' : '-';
    if (mode & S_ISUID)
        buf[2] = mode & S_IXUSR ? 's' : 'S';
    else
        buf[2] = mode & S_IXUSR ? 'x' : '-';
    buf[3] = mode & S_IRGRP ? 'r' : '-';
    buf[4] = mode & S_IWGRP ? 'w' : '-';
    if (mode & S_ISGID)
        buf[5] = mode & S_IXGRP ? 's' : 'S';
    else
        buf[5] = mode & S_IXGRP ? 'x' : '-';
    buf[6] = mode & S_IROTH ? 'r' : '-';
    buf[7] = mode & S_IWOTH ? 'w' : '-';
    if (mode & S_ISVTX)
        buf[8] = mode & S_IXOTH ? 't' : 'T';
    else
        buf[8] = mode & S_IXOTH ? 'x' : '-';
}

static JSValue qp_stat_filemode(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                int nargs, JSValueConst kwnames, int magic)
{
    char buf[10];
    mode_t mode;
    if (qp_stat_as_mode(ctx, args[0], &mode) < 0)
        return JS_EXCEPTION;
    buf[0] = qp_stat_filetype(mode);
    qp_stat_fileperm(mode, &buf[1]);
    return qp_str_from_utf8(ctx, buf, 10);
}

static const QPMethodDef qp_stat_methods[] = {
    { "S_ISDIR", qp_stat_isfunc, QP_METH_O, QP_STAT_S_ISDIR, "S_ISDIR(mode) -> bool\n\n"
    "Return True if mode is from a directory." },
    { "S_ISCHR", qp_stat_isfunc, QP_METH_O, QP_STAT_S_ISCHR, "S_ISCHR(mode) -> bool\n\n"
    "Return True if mode is from a character special device file." },
    { "S_ISBLK", qp_stat_isfunc, QP_METH_O, QP_STAT_S_ISBLK, "S_ISBLK(mode) -> bool\n\n"
    "Return True if mode is from a block special device file." },
    { "S_ISREG", qp_stat_isfunc, QP_METH_O, QP_STAT_S_ISREG, "S_ISREG(mode) -> bool\n\n"
    "Return True if mode is from a regular file." },
    { "S_ISFIFO", qp_stat_isfunc, QP_METH_O, QP_STAT_S_ISFIFO, "S_ISFIFO(mode) -> bool\n\n"
    "Return True if mode is from a FIFO (named pipe)." },
    { "S_ISLNK", qp_stat_isfunc, QP_METH_O, QP_STAT_S_ISLNK, "S_ISLNK(mode) -> bool\n\n"
    "Return True if mode is from a symbolic link." },
    { "S_ISSOCK", qp_stat_isfunc, QP_METH_O, QP_STAT_S_ISSOCK, "S_ISSOCK(mode) -> bool\n\n"
    "Return True if mode is from a socket." },
    { "S_ISDOOR", qp_stat_isfunc, QP_METH_O, QP_STAT_S_ISDOOR, "S_ISDOOR(mode) -> bool\n\n"
    "Return True if mode is from a door." },
    { "S_ISPORT", qp_stat_isfunc, QP_METH_O, QP_STAT_S_ISPORT, "S_ISPORT(mode) -> bool\n\n"
    "Return True if mode is from an event port." },
    { "S_ISWHT", qp_stat_isfunc, QP_METH_O, QP_STAT_S_ISWHT, "S_ISWHT(mode) -> bool\n\n"
    "Return True if mode is from a whiteout." },
    { "S_IMODE", qp_stat_S_IMODE, QP_METH_O, 0, "Return the portion of the file's mode that can be set by os.chmod()." },
    { "S_IFMT", qp_stat_S_IMODE, QP_METH_O, 1, "Return the portion of the file's mode that describes the file type." },
    { "filemode", qp_stat_filemode, QP_METH_O, 0, "Convert a file's mode to a string of the form '-rwxrwxrwx'" },
    { NULL }
};

static int qp_stat_add_int(JSContext *ctx, JSValueConst module, const char *name, int64_t v)
{
    JSAtom at = JS_NewAtom(ctx, name);
    JSValue val = qp_long_from_int64(ctx, v);
    int r = JS_IsException(val) ? -1 : qp_dict_set_item_atom(ctx, qp_module_get_dict(module), at, val);
    JS_FreeAtom(ctx, at);
    JS_FreeValue(ctx, val);
    return r;
}

static int qp_stat_exec(JSContext *ctx, JSValueConst module)
{
    static const char *const st_constants[] = {
        "ST_MODE", "ST_INO", "ST_DEV", "ST_NLINK", "ST_UID", "ST_GID",
        "ST_SIZE", "ST_ATIME", "ST_MTIME", "ST_CTIME"
    };
    int i;
#define QP_STAT_ADD(macro) \
    do { if (qp_stat_add_int(ctx, module, #macro, (int64_t)(macro)) < 0) return -1; } while (0)
    QP_STAT_ADD(S_IFDIR);
    QP_STAT_ADD(S_IFCHR);
    QP_STAT_ADD(S_IFBLK);
    QP_STAT_ADD(S_IFREG);
    QP_STAT_ADD(S_IFIFO);
    QP_STAT_ADD(S_IFLNK);
    QP_STAT_ADD(S_IFSOCK);
    QP_STAT_ADD(S_IFDOOR);
    QP_STAT_ADD(S_IFPORT);
    QP_STAT_ADD(S_IFWHT);

    QP_STAT_ADD(S_ISUID);
    QP_STAT_ADD(S_ISGID);
    QP_STAT_ADD(S_ISVTX);
    QP_STAT_ADD(S_ENFMT);

    QP_STAT_ADD(S_IREAD);
    QP_STAT_ADD(S_IWRITE);
    QP_STAT_ADD(S_IEXEC);

    QP_STAT_ADD(S_IRWXU);
    QP_STAT_ADD(S_IRUSR);
    QP_STAT_ADD(S_IWUSR);
    QP_STAT_ADD(S_IXUSR);

    QP_STAT_ADD(S_IRWXG);
    QP_STAT_ADD(S_IRGRP);
    QP_STAT_ADD(S_IWGRP);
    QP_STAT_ADD(S_IXGRP);

    QP_STAT_ADD(S_IRWXO);
    QP_STAT_ADD(S_IROTH);
    QP_STAT_ADD(S_IWOTH);
    QP_STAT_ADD(S_IXOTH);

    QP_STAT_ADD(UF_SETTABLE);
    QP_STAT_ADD(UF_NODUMP);
    QP_STAT_ADD(UF_IMMUTABLE);
    QP_STAT_ADD(UF_APPEND);
    QP_STAT_ADD(UF_OPAQUE);
    QP_STAT_ADD(UF_NOUNLINK);
    QP_STAT_ADD(UF_COMPRESSED);
    QP_STAT_ADD(UF_TRACKED);
    QP_STAT_ADD(UF_DATAVAULT);
    QP_STAT_ADD(UF_HIDDEN);
    QP_STAT_ADD(SF_SETTABLE);
    QP_STAT_ADD(SF_ARCHIVED);
    QP_STAT_ADD(SF_IMMUTABLE);
    QP_STAT_ADD(SF_APPEND);
    QP_STAT_ADD(SF_NOUNLINK);
    QP_STAT_ADD(SF_SNAPSHOT);
    QP_STAT_ADD(SF_FIRMLINK);
    QP_STAT_ADD(SF_DATALESS);

#ifdef SF_SUPPORTED
    QP_STAT_ADD(SF_SUPPORTED);
#endif
#ifdef SF_SYNTHETIC
    QP_STAT_ADD(SF_SYNTHETIC);
#endif



#undef QP_STAT_ADD
    for (i = 0; i < (int)countof(st_constants); i++)
        if (qp_stat_add_int(ctx, module, st_constants[i], i) < 0)
            return -1;
    return 0;
}

static const QPModuleDef qp_moddef__stat = {
    "_stat",
"S_IFMT_: file type bits\n\
S_IFDIR: directory\n\
S_IFCHR: character device\n\
S_IFBLK: block device\n\
S_IFREG: regular file\n\
S_IFIFO: fifo (named pipe)\n\
S_IFLNK: symbolic link\n\
S_IFSOCK: socket file\n\
S_IFDOOR: door\n\
S_IFPORT: event port\n\
S_IFWHT: whiteout\n\
\n"

"S_ISUID: set UID bit\n\
S_ISGID: set GID bit\n\
S_ENFMT: file locking enforcement\n\
S_ISVTX: sticky bit\n\
S_IREAD: Unix V7 synonym for S_IRUSR\n\
S_IWRITE: Unix V7 synonym for S_IWUSR\n\
S_IEXEC: Unix V7 synonym for S_IXUSR\n\
S_IRWXU: mask for owner permissions\n\
S_IRUSR: read by owner\n\
S_IWUSR: write by owner\n\
S_IXUSR: execute by owner\n\
S_IRWXG: mask for group permissions\n\
S_IRGRP: read by group\n\
S_IWGRP: write by group\n\
S_IXGRP: execute by group\n\
S_IRWXO: mask for others (not in group) permissions\n\
S_IROTH: read by others\n\
S_IWOTH: write by others\n\
S_IXOTH: execute by others\n\
\n"

"UF_SETTABLE: mask of owner changeable flags\n\
UF_NODUMP: do not dump file\n\
UF_IMMUTABLE: file may not be changed\n\
UF_APPEND: file may only be appended to\n\
UF_OPAQUE: directory is opaque when viewed through a union stack\n\
UF_NOUNLINK: file may not be renamed or deleted\n\
UF_COMPRESSED: macOS: file is hfs-compressed\n\
UF_TRACKED: used for dealing with document IDs\n\
UF_DATAVAULT: entitlement required for reading and writing\n\
UF_HIDDEN: macOS: file should not be displayed\n\
SF_SETTABLE: mask of super user changeable flags\n\
SF_ARCHIVED: file may be archived\n\
SF_IMMUTABLE: file may not be changed\n\
SF_APPEND: file may only be appended to\n\
SF_RESTRICTED: entitlement required for writing\n\
SF_NOUNLINK: file may not be renamed or deleted\n\
SF_SNAPSHOT: file is a snapshot file\n\
SF_FIRMLINK: file is a firmlink\n\
SF_DATALESS: file is a dataless object\n\
\n\
On macOS:\n\
SF_SUPPORTED: mask of super user supported flags\n\
SF_SYNTHETIC: mask of read-only synthetic flags\n\
\n"

"ST_MODE\n\
ST_INO\n\
ST_DEV\n\
ST_NLINK\n\
ST_UID\n\
ST_GID\n\
ST_SIZE\n\
ST_ATIME\n\
ST_MTIME\n\
ST_CTIME\n\
\n"

"FILE_ATTRIBUTE_*: Windows file attribute constants\n\
                   (only present on Windows)\n\
",
    qp_stat_methods,
    qp_stat_exec,
};
