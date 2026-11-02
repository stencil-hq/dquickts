/*
 * errno module (port of CPython Modules/errnomodule.c, PSF-2.0).
 */
#include <errno.h>

typedef struct QPErrnoEntry {
    const char *name;
    int code;
} QPErrnoEntry;

static const QPErrnoEntry qp_errno_table[] = {
#ifdef ENODEV
    {"ENODEV", ENODEV},
#endif
#ifdef ENOCSI
    {"ENOCSI", ENOCSI},
#endif
#ifdef EHOSTUNREACH
    {"EHOSTUNREACH", EHOSTUNREACH},
#else
#ifdef WSAEHOSTUNREACH
    {"EHOSTUNREACH", WSAEHOSTUNREACH},
#endif
#endif
#ifdef ENOMSG
    {"ENOMSG", ENOMSG},
#endif
#ifdef EUCLEAN
    {"EUCLEAN", EUCLEAN},
#endif
#ifdef EL2NSYNC
    {"EL2NSYNC", EL2NSYNC},
#endif
#ifdef EL2HLT
    {"EL2HLT", EL2HLT},
#endif
#ifdef ENODATA
    {"ENODATA", ENODATA},
#endif
#ifdef ENOTBLK
    {"ENOTBLK", ENOTBLK},
#endif
#ifdef ENOSYS
    {"ENOSYS", ENOSYS},
#endif
#ifdef EPIPE
    {"EPIPE", EPIPE},
#endif
#ifdef EINVAL
    {"EINVAL", EINVAL},
#else
#ifdef WSAEINVAL
    {"EINVAL", WSAEINVAL},
#endif
#endif
#ifdef EOVERFLOW
    {"EOVERFLOW", EOVERFLOW},
#endif
#ifdef EADV
    {"EADV", EADV},
#endif
#ifdef EINTR
    {"EINTR", EINTR},
#else
#ifdef WSAEINTR
    {"EINTR", WSAEINTR},
#endif
#endif
#ifdef EUSERS
    {"EUSERS", EUSERS},
#else
#ifdef WSAEUSERS
    {"EUSERS", WSAEUSERS},
#endif
#endif
#ifdef ENOTEMPTY
    {"ENOTEMPTY", ENOTEMPTY},
#else
#ifdef WSAENOTEMPTY
    {"ENOTEMPTY", WSAENOTEMPTY},
#endif
#endif
#ifdef ENOBUFS
    {"ENOBUFS", ENOBUFS},
#else
#ifdef WSAENOBUFS
    {"ENOBUFS", WSAENOBUFS},
#endif
#endif
#ifdef EPROTO
    {"EPROTO", EPROTO},
#endif
#ifdef EREMOTE
    {"EREMOTE", EREMOTE},
#else
#ifdef WSAEREMOTE
    {"EREMOTE", WSAEREMOTE},
#endif
#endif
#ifdef ENAVAIL
    {"ENAVAIL", ENAVAIL},
#endif
#ifdef ECHILD
    {"ECHILD", ECHILD},
#endif
#ifdef ELOOP
    {"ELOOP", ELOOP},
#else
#ifdef WSAELOOP
    {"ELOOP", WSAELOOP},
#endif
#endif
#ifdef EXDEV
    {"EXDEV", EXDEV},
#endif
#ifdef E2BIG
    {"E2BIG", E2BIG},
#endif
#ifdef ESRCH
    {"ESRCH", ESRCH},
#endif
#ifdef EMSGSIZE
    {"EMSGSIZE", EMSGSIZE},
#else
#ifdef WSAEMSGSIZE
    {"EMSGSIZE", WSAEMSGSIZE},
#endif
#endif
#ifdef EAFNOSUPPORT
    {"EAFNOSUPPORT", EAFNOSUPPORT},
#else
#ifdef WSAEAFNOSUPPORT
    {"EAFNOSUPPORT", WSAEAFNOSUPPORT},
#endif
#endif
#ifdef EBADR
    {"EBADR", EBADR},
#endif
#ifdef EHOSTDOWN
    {"EHOSTDOWN", EHOSTDOWN},
#else
#ifdef WSAEHOSTDOWN
    {"EHOSTDOWN", WSAEHOSTDOWN},
#endif
#endif
#ifdef EPFNOSUPPORT
    {"EPFNOSUPPORT", EPFNOSUPPORT},
#else
#ifdef WSAEPFNOSUPPORT
    {"EPFNOSUPPORT", WSAEPFNOSUPPORT},
#endif
#endif
#ifdef ENOPROTOOPT
    {"ENOPROTOOPT", ENOPROTOOPT},
#else
#ifdef WSAENOPROTOOPT
    {"ENOPROTOOPT", WSAENOPROTOOPT},
#endif
#endif
#ifdef EBUSY
    {"EBUSY", EBUSY},
#endif
#ifdef EWOULDBLOCK
    {"EWOULDBLOCK", EWOULDBLOCK},
#else
#ifdef WSAEWOULDBLOCK
    {"EWOULDBLOCK", WSAEWOULDBLOCK},
#endif
#endif
#ifdef EBADFD
    {"EBADFD", EBADFD},
#endif
#ifdef EDOTDOT
    {"EDOTDOT", EDOTDOT},
#endif
#ifdef EISCONN
    {"EISCONN", EISCONN},
#else
#ifdef WSAEISCONN
    {"EISCONN", WSAEISCONN},
#endif
#endif
#ifdef ENOANO
    {"ENOANO", ENOANO},
#endif
#if defined(__wasi__) && !defined(ESHUTDOWN)
#define ESHUTDOWN EPIPE
#endif
#ifdef ESHUTDOWN
    {"ESHUTDOWN", ESHUTDOWN},
#else
#ifdef WSAESHUTDOWN
    {"ESHUTDOWN", WSAESHUTDOWN},
#endif
#endif
#ifdef ECHRNG
    {"ECHRNG", ECHRNG},
#endif
#ifdef ELIBBAD
    {"ELIBBAD", ELIBBAD},
#endif
#ifdef ENONET
    {"ENONET", ENONET},
#endif
#ifdef EBADE
    {"EBADE", EBADE},
#endif
#ifdef EBADF
    {"EBADF", EBADF},
#else
#ifdef WSAEBADF
    {"EBADF", WSAEBADF},
#endif
#endif
#ifdef EMULTIHOP
    {"EMULTIHOP", EMULTIHOP},
#endif
#ifdef EIO
    {"EIO", EIO},
#endif
#ifdef EUNATCH
    {"EUNATCH", EUNATCH},
#endif
#ifdef EPROTOTYPE
    {"EPROTOTYPE", EPROTOTYPE},
#else
#ifdef WSAEPROTOTYPE
    {"EPROTOTYPE", WSAEPROTOTYPE},
#endif
#endif
#ifdef ENOSPC
    {"ENOSPC", ENOSPC},
#endif
#ifdef ENOEXEC
    {"ENOEXEC", ENOEXEC},
#endif
#ifdef EALREADY
    {"EALREADY", EALREADY},
#else
#ifdef WSAEALREADY
    {"EALREADY", WSAEALREADY},
#endif
#endif
#ifdef ENETDOWN
    {"ENETDOWN", ENETDOWN},
#else
#ifdef WSAENETDOWN
    {"ENETDOWN", WSAENETDOWN},
#endif
#endif
#ifdef ENOTNAM
    {"ENOTNAM", ENOTNAM},
#endif
#ifdef EACCES
    {"EACCES", EACCES},
#else
#ifdef WSAEACCES
    {"EACCES", WSAEACCES},
#endif
#endif
#ifdef ELNRNG
    {"ELNRNG", ELNRNG},
#endif
#ifdef EILSEQ
    {"EILSEQ", EILSEQ},
#endif
#ifdef ENOTDIR
    {"ENOTDIR", ENOTDIR},
#endif
#ifdef ENOTUNIQ
    {"ENOTUNIQ", ENOTUNIQ},
#endif
#ifdef EPERM
    {"EPERM", EPERM},
#endif
#ifdef EDOM
    {"EDOM", EDOM},
#endif
#ifdef EXFULL
    {"EXFULL", EXFULL},
#endif
#ifdef ECONNREFUSED
    {"ECONNREFUSED", ECONNREFUSED},
#else
#ifdef WSAECONNREFUSED
    {"ECONNREFUSED", WSAECONNREFUSED},
#endif
#endif
#ifdef EISDIR
    {"EISDIR", EISDIR},
#endif
#ifdef EPROTONOSUPPORT
    {"EPROTONOSUPPORT", EPROTONOSUPPORT},
#else
#ifdef WSAEPROTONOSUPPORT
    {"EPROTONOSUPPORT", WSAEPROTONOSUPPORT},
#endif
#endif
#ifdef EROFS
    {"EROFS", EROFS},
#endif
#ifdef EADDRNOTAVAIL
    {"EADDRNOTAVAIL", EADDRNOTAVAIL},
#else
#ifdef WSAEADDRNOTAVAIL
    {"EADDRNOTAVAIL", WSAEADDRNOTAVAIL},
#endif
#endif
#ifdef EIDRM
    {"EIDRM", EIDRM},
#endif
#ifdef ECOMM
    {"ECOMM", ECOMM},
#endif
#ifdef ESRMNT
    {"ESRMNT", ESRMNT},
#endif
#ifdef EREMOTEIO
    {"EREMOTEIO", EREMOTEIO},
#endif
#ifdef EL3RST
    {"EL3RST", EL3RST},
#endif
#ifdef EBADMSG
    {"EBADMSG", EBADMSG},
#endif
#ifdef ENFILE
    {"ENFILE", ENFILE},
#endif
#ifdef ELIBMAX
    {"ELIBMAX", ELIBMAX},
#endif
#ifdef ESPIPE
    {"ESPIPE", ESPIPE},
#endif
#ifdef ENOLINK
    {"ENOLINK", ENOLINK},
#endif
#ifdef ENETRESET
    {"ENETRESET", ENETRESET},
#else
#ifdef WSAENETRESET
    {"ENETRESET", WSAENETRESET},
#endif
#endif
#ifdef ETIMEDOUT
    {"ETIMEDOUT", ETIMEDOUT},
#else
#ifdef WSAETIMEDOUT
    {"ETIMEDOUT", WSAETIMEDOUT},
#endif
#endif
#ifdef ENOENT
    {"ENOENT", ENOENT},
#endif
#ifdef EEXIST
    {"EEXIST", EEXIST},
#endif
#ifdef EDQUOT
    {"EDQUOT", EDQUOT},
#else
#ifdef WSAEDQUOT
    {"EDQUOT", WSAEDQUOT},
#endif
#endif
#ifdef ENOSTR
    {"ENOSTR", ENOSTR},
#endif
#ifdef EBADSLT
    {"EBADSLT", EBADSLT},
#endif
#ifdef EBADRQC
    {"EBADRQC", EBADRQC},
#endif
#ifdef ELIBACC
    {"ELIBACC", ELIBACC},
#endif
#ifdef EFAULT
    {"EFAULT", EFAULT},
#else
#ifdef WSAEFAULT
    {"EFAULT", WSAEFAULT},
#endif
#endif
#ifdef EFBIG
    {"EFBIG", EFBIG},
#endif
#ifdef EDEADLK
    {"EDEADLK", EDEADLK},
#endif
#ifdef ENOTCONN
    {"ENOTCONN", ENOTCONN},
#else
#ifdef WSAENOTCONN
    {"ENOTCONN", WSAENOTCONN},
#endif
#endif
#ifdef EDESTADDRREQ
    {"EDESTADDRREQ", EDESTADDRREQ},
#else
#ifdef WSAEDESTADDRREQ
    {"EDESTADDRREQ", WSAEDESTADDRREQ},
#endif
#endif
#ifdef ELIBSCN
    {"ELIBSCN", ELIBSCN},
#endif
#ifdef ENOLCK
    {"ENOLCK", ENOLCK},
#endif
#ifdef EISNAM
    {"EISNAM", EISNAM},
#endif
#ifdef ECONNABORTED
    {"ECONNABORTED", ECONNABORTED},
#else
#ifdef WSAECONNABORTED
    {"ECONNABORTED", WSAECONNABORTED},
#endif
#endif
#ifdef ENETUNREACH
    {"ENETUNREACH", ENETUNREACH},
#else
#ifdef WSAENETUNREACH
    {"ENETUNREACH", WSAENETUNREACH},
#endif
#endif
#ifdef ESTALE
    {"ESTALE", ESTALE},
#else
#ifdef WSAESTALE
    {"ESTALE", WSAESTALE},
#endif
#endif
#ifdef ENOSR
    {"ENOSR", ENOSR},
#endif
#ifdef ENOMEM
    {"ENOMEM", ENOMEM},
#endif
#ifdef ENOTSOCK
    {"ENOTSOCK", ENOTSOCK},
#else
#ifdef WSAENOTSOCK
    {"ENOTSOCK", WSAENOTSOCK},
#endif
#endif
#ifdef ESTRPIPE
    {"ESTRPIPE", ESTRPIPE},
#endif
#ifdef EMLINK
    {"EMLINK", EMLINK},
#endif
#ifdef ERANGE
    {"ERANGE", ERANGE},
#endif
#ifdef ELIBEXEC
    {"ELIBEXEC", ELIBEXEC},
#endif
#ifdef EL3HLT
    {"EL3HLT", EL3HLT},
#endif
#ifdef ECONNRESET
    {"ECONNRESET", ECONNRESET},
#else
#ifdef WSAECONNRESET
    {"ECONNRESET", WSAECONNRESET},
#endif
#endif
#ifdef EADDRINUSE
    {"EADDRINUSE", EADDRINUSE},
#else
#ifdef WSAEADDRINUSE
    {"EADDRINUSE", WSAEADDRINUSE},
#endif
#endif
#ifdef EOPNOTSUPP
    {"EOPNOTSUPP", EOPNOTSUPP},
#else
#ifdef WSAEOPNOTSUPP
    {"EOPNOTSUPP", WSAEOPNOTSUPP},
#endif
#endif
#ifdef EREMCHG
    {"EREMCHG", EREMCHG},
#endif
#ifdef EAGAIN
    {"EAGAIN", EAGAIN},
#endif
#ifdef ENAMETOOLONG
    {"ENAMETOOLONG", ENAMETOOLONG},
#else
#ifdef WSAENAMETOOLONG
    {"ENAMETOOLONG", WSAENAMETOOLONG},
#endif
#endif
#ifdef ENOTTY
    {"ENOTTY", ENOTTY},
#endif
#ifdef ERESTART
    {"ERESTART", ERESTART},
#endif
#ifdef ESOCKTNOSUPPORT
    {"ESOCKTNOSUPPORT", ESOCKTNOSUPPORT},
#else
#ifdef WSAESOCKTNOSUPPORT
    {"ESOCKTNOSUPPORT", WSAESOCKTNOSUPPORT},
#endif
#endif
#ifdef ETIME
    {"ETIME", ETIME},
#endif
#ifdef EBFONT
    {"EBFONT", EBFONT},
#endif
#ifdef EDEADLOCK
    {"EDEADLOCK", EDEADLOCK},
#endif
#ifdef ETOOMANYREFS
    {"ETOOMANYREFS", ETOOMANYREFS},
#else
#ifdef WSAETOOMANYREFS
    {"ETOOMANYREFS", WSAETOOMANYREFS},
#endif
#endif
#ifdef EMFILE
    {"EMFILE", EMFILE},
#else
#ifdef WSAEMFILE
    {"EMFILE", WSAEMFILE},
#endif
#endif
#ifdef ETXTBSY
    {"ETXTBSY", ETXTBSY},
#endif
#ifdef EINPROGRESS
    {"EINPROGRESS", EINPROGRESS},
#else
#ifdef WSAEINPROGRESS
    {"EINPROGRESS", WSAEINPROGRESS},
#endif
#endif
#ifdef ENXIO
    {"ENXIO", ENXIO},
#endif
#ifdef ENOPKG
    {"ENOPKG", ENOPKG},
#endif
#ifdef WSASY
    {"WSASY", WSASY},
#endif
#ifdef WSAEHOSTDOWN
    {"WSAEHOSTDOWN", WSAEHOSTDOWN},
#endif
#ifdef WSAENETDOWN
    {"WSAENETDOWN", WSAENETDOWN},
#endif
#ifdef WSAENOTSOCK
    {"WSAENOTSOCK", WSAENOTSOCK},
#endif
#ifdef WSAEHOSTUNREACH
    {"WSAEHOSTUNREACH", WSAEHOSTUNREACH},
#endif
#ifdef WSAELOOP
    {"WSAELOOP", WSAELOOP},
#endif
#ifdef WSAEMFILE
    {"WSAEMFILE", WSAEMFILE},
#endif
#ifdef WSAESTALE
    {"WSAESTALE", WSAESTALE},
#endif
#ifdef WSAVERNOTSUPPORTED
    {"WSAVERNOTSUPPORTED", WSAVERNOTSUPPORTED},
#endif
#ifdef WSAENETUNREACH
    {"WSAENETUNREACH", WSAENETUNREACH},
#endif
#ifdef WSAEPROCLIM
    {"WSAEPROCLIM", WSAEPROCLIM},
#endif
#ifdef WSAEFAULT
    {"WSAEFAULT", WSAEFAULT},
#endif
#ifdef WSANOTINITIALISED
    {"WSANOTINITIALISED", WSANOTINITIALISED},
#endif
#ifdef WSAEUSERS
    {"WSAEUSERS", WSAEUSERS},
#endif
#ifdef WSAMAKEASYNCREPL
    {"WSAMAKEASYNCREPL", WSAMAKEASYNCREPL},
#endif
#ifdef WSAENOPROTOOPT
    {"WSAENOPROTOOPT", WSAENOPROTOOPT},
#endif
#ifdef WSAECONNABORTED
    {"WSAECONNABORTED", WSAECONNABORTED},
#endif
#ifdef WSAENAMETOOLONG
    {"WSAENAMETOOLONG", WSAENAMETOOLONG},
#endif
#ifdef WSAENOTEMPTY
    {"WSAENOTEMPTY", WSAENOTEMPTY},
#endif
#ifdef WSAESHUTDOWN
    {"WSAESHUTDOWN", WSAESHUTDOWN},
#endif
#ifdef WSAEAFNOSUPPORT
    {"WSAEAFNOSUPPORT", WSAEAFNOSUPPORT},
#endif
#ifdef WSAETOOMANYREFS
    {"WSAETOOMANYREFS", WSAETOOMANYREFS},
#endif
#ifdef WSAEACCES
    {"WSAEACCES", WSAEACCES},
#endif
#ifdef WSATR
    {"WSATR", WSATR},
#endif
#ifdef WSABASEERR
    {"WSABASEERR", WSABASEERR},
#endif
#ifdef WSADESCRIPTIO
    {"WSADESCRIPTIO", WSADESCRIPTIO},
#endif
#ifdef WSAEMSGSIZE
    {"WSAEMSGSIZE", WSAEMSGSIZE},
#endif
#ifdef WSAEBADF
    {"WSAEBADF", WSAEBADF},
#endif
#ifdef WSAECONNRESET
    {"WSAECONNRESET", WSAECONNRESET},
#endif
#ifdef WSAGETSELECTERRO
    {"WSAGETSELECTERRO", WSAGETSELECTERRO},
#endif
#ifdef WSAETIMEDOUT
    {"WSAETIMEDOUT", WSAETIMEDOUT},
#endif
#ifdef WSAENOBUFS
    {"WSAENOBUFS", WSAENOBUFS},
#endif
#ifdef WSAEDISCON
    {"WSAEDISCON", WSAEDISCON},
#endif
#ifdef WSAEINTR
    {"WSAEINTR", WSAEINTR},
#endif
#ifdef WSAEPROTOTYPE
    {"WSAEPROTOTYPE", WSAEPROTOTYPE},
#endif
#ifdef WSAHOS
    {"WSAHOS", WSAHOS},
#endif
#ifdef WSAEADDRINUSE
    {"WSAEADDRINUSE", WSAEADDRINUSE},
#endif
#ifdef WSAEADDRNOTAVAIL
    {"WSAEADDRNOTAVAIL", WSAEADDRNOTAVAIL},
#endif
#ifdef WSAEALREADY
    {"WSAEALREADY", WSAEALREADY},
#endif
#ifdef WSAEPROTONOSUPPORT
    {"WSAEPROTONOSUPPORT", WSAEPROTONOSUPPORT},
#endif
#ifdef WSASYSNOTREADY
    {"WSASYSNOTREADY", WSASYSNOTREADY},
#endif
#ifdef WSAEWOULDBLOCK
    {"WSAEWOULDBLOCK", WSAEWOULDBLOCK},
#endif
#ifdef WSAEPFNOSUPPORT
    {"WSAEPFNOSUPPORT", WSAEPFNOSUPPORT},
#endif
#ifdef WSAEOPNOTSUPP
    {"WSAEOPNOTSUPP", WSAEOPNOTSUPP},
#endif
#ifdef WSAEISCONN
    {"WSAEISCONN", WSAEISCONN},
#endif
#ifdef WSAEDQUOT
    {"WSAEDQUOT", WSAEDQUOT},
#endif
#ifdef WSAENOTCONN
    {"WSAENOTCONN", WSAENOTCONN},
#endif
#ifdef WSAEREMOTE
    {"WSAEREMOTE", WSAEREMOTE},
#endif
#ifdef WSAEINVAL
    {"WSAEINVAL", WSAEINVAL},
#endif
#ifdef WSAEINPROGRESS
    {"WSAEINPROGRESS", WSAEINPROGRESS},
#endif
#ifdef WSAGETSELECTEVEN
    {"WSAGETSELECTEVEN", WSAGETSELECTEVEN},
#endif
#ifdef WSAESOCKTNOSUPPORT
    {"WSAESOCKTNOSUPPORT", WSAESOCKTNOSUPPORT},
#endif
#ifdef WSAGETASYNCERRO
    {"WSAGETASYNCERRO", WSAGETASYNCERRO},
#endif
#ifdef WSAMAKESELECTREPL
    {"WSAMAKESELECTREPL", WSAMAKESELECTREPL},
#endif
#ifdef WSAGETASYNCBUFLE
    {"WSAGETASYNCBUFLE", WSAGETASYNCBUFLE},
#endif
#ifdef WSAEDESTADDRREQ
    {"WSAEDESTADDRREQ", WSAEDESTADDRREQ},
#endif
#ifdef WSAECONNREFUSED
    {"WSAECONNREFUSED", WSAECONNREFUSED},
#endif
#ifdef WSAENETRESET
    {"WSAENETRESET", WSAENETRESET},
#endif
#ifdef WSAN
    {"WSAN", WSAN},
#endif
#ifdef ENOMEDIUM
    {"ENOMEDIUM", ENOMEDIUM},
#endif
#ifdef EMEDIUMTYPE
    {"EMEDIUMTYPE", EMEDIUMTYPE},
#endif
#ifdef ECANCELED
    {"ECANCELED", ECANCELED},
#endif
#ifdef ENOKEY
    {"ENOKEY", ENOKEY},
#endif
#ifdef EHWPOISON
    {"EHWPOISON", EHWPOISON},
#endif
#ifdef EKEYEXPIRED
    {"EKEYEXPIRED", EKEYEXPIRED},
#endif
#ifdef EKEYREVOKED
    {"EKEYREVOKED", EKEYREVOKED},
#endif
#ifdef EKEYREJECTED
    {"EKEYREJECTED", EKEYREJECTED},
#endif
#ifdef EOWNERDEAD
    {"EOWNERDEAD", EOWNERDEAD},
#endif
#ifdef ENOTRECOVERABLE
    {"ENOTRECOVERABLE", ENOTRECOVERABLE},
#endif
#ifdef ERFKILL
    {"ERFKILL", ERFKILL},
#endif
#ifdef ECANCELED
    {"ECANCELED", ECANCELED},
#endif
#ifdef ENOTSUP
    {"ENOTSUP", ENOTSUP},
#endif
#ifdef EOWNERDEAD
    {"EOWNERDEAD", EOWNERDEAD},
#endif
#ifdef ENOTRECOVERABLE
    {"ENOTRECOVERABLE", ENOTRECOVERABLE},
#endif
#ifdef ELOCKUNMAPPED
    {"ELOCKUNMAPPED", ELOCKUNMAPPED},
#endif
#ifdef ENOTACTIVE
    {"ENOTACTIVE", ENOTACTIVE},
#endif
#ifdef EAUTH
    {"EAUTH", EAUTH},
#endif
#ifdef EBADARCH
    {"EBADARCH", EBADARCH},
#endif
#ifdef EBADEXEC
    {"EBADEXEC", EBADEXEC},
#endif
#ifdef EBADMACHO
    {"EBADMACHO", EBADMACHO},
#endif
#ifdef EBADRPC
    {"EBADRPC", EBADRPC},
#endif
#ifdef EDEVERR
    {"EDEVERR", EDEVERR},
#endif
#ifdef EFTYPE
    {"EFTYPE", EFTYPE},
#endif
#ifdef ENEEDAUTH
    {"ENEEDAUTH", ENEEDAUTH},
#endif
#ifdef ENOATTR
    {"ENOATTR", ENOATTR},
#endif
#ifdef ENOPOLICY
    {"ENOPOLICY", ENOPOLICY},
#endif
#ifdef EPROCLIM
    {"EPROCLIM", EPROCLIM},
#endif
#ifdef EPROCUNAVAIL
    {"EPROCUNAVAIL", EPROCUNAVAIL},
#endif
#ifdef EPROGMISMATCH
    {"EPROGMISMATCH", EPROGMISMATCH},
#endif
#ifdef EPROGUNAVAIL
    {"EPROGUNAVAIL", EPROGUNAVAIL},
#endif
#ifdef EPWROFF
    {"EPWROFF", EPWROFF},
#endif
#ifdef ERPCMISMATCH
    {"ERPCMISMATCH", ERPCMISMATCH},
#endif
#ifdef ESHLIBVERS
    {"ESHLIBVERS", ESHLIBVERS},
#endif
#ifdef EQFULL
    {"EQFULL", EQFULL},
#endif
#ifdef ENOTCAPABLE
    {"ENOTCAPABLE", ENOTCAPABLE},
#endif
    {NULL, 0}
};

static int qp_errno_exec(JSContext *ctx, JSValueConst module)
{
    JSValue mdict = qp_module_get_dict(module);
    JSValue error_dict = qp_dict_new(ctx);
    JSAtom at;
    int i, r;

    if (JS_IsException(error_dict))
        return -1;
    at = JS_NewAtom(ctx, "errorcode");
    r = qp_dict_set_item_atom(ctx, mdict, at, error_dict);
    JS_FreeAtom(ctx, at);
    if (r < 0)
        goto fail;
    for (i = 0; qp_errno_table[i].name; i++) {
        JSValue name = qp_str_from_cstr(ctx, qp_errno_table[i].name);
        JSValue code = JS_NewInt32(ctx, qp_errno_table[i].code);
        if (JS_IsException(name))
            goto fail;
        /* insert in module dict and errorcode dict (_add_errcode) */
        if (qp_dict_set_item(ctx, mdict, name, code) < 0 ||
            qp_dict_set_item(ctx, error_dict, code, name) < 0) {
            JS_FreeValue(ctx, name);
            goto fail;
        }
        JS_FreeValue(ctx, name);
    }
    JS_FreeValue(ctx, error_dict);
    return 0;
fail:
    JS_FreeValue(ctx, error_dict);
    return -1;
}

static const QPModuleDef qp_moddef_errno = {
    "errno",
"This module makes available standard errno system symbols.\n\
\n\
The value of each symbol is the corresponding integer value,\n\
e.g., on most systems, errno.ENOENT equals the integer 2.\n\
\n\
The dictionary errno.errorcode maps numeric codes to symbol names,\n\
e.g., errno.errorcode[2] could be the string 'ENOENT'.\n\
\n\
Symbols that are not relevant to the underlying system are not defined.\n\
\n\
To map error codes to error messages, use the function os.strerror(),\n\
e.g. os.strerror(2) could return 'No such file or directory'.",
    NULL,
    qp_errno_exec,
};
