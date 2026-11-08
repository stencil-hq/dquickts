/*
 * qbun:os native binding: the system information of node:os (libuv) and
 * the tables of os.constants. Also the signal table shared with
 * node/process.c (qb_signal_name(), qb_signals_object()).
 */
#include <errno.h>
#include <signal.h>
#include <dlfcn.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <netinet/in.h>
#include "qbun.h"

/* shared with node/process.c (to be declared in qbun.h) */
const char *qb_signal_name(int signum);
JSValue qb_signals_object(JSContext *ctx);

typedef struct QBConst {
    const char *name;
    int num;
} QBConst;

/* an object { name: num, ... } of a constant table */
static JSValue qb_table_object(JSContext *ctx, const QBConst *t, size_t n)
{
    JSValue obj = JS_NewObject(ctx);
    size_t i;
    if (JS_IsException(obj))
        return obj;
    for (i = 0; i < n; i++) {
        if (qb_set(ctx, obj, t[i].name, JS_NewInt32(ctx, t[i].num)) < 0) {
            JS_FreeValue(ctx, obj);
            return JS_EXCEPTION;
        }
    }
    return obj;
}

/* the signal table, in the order of Node's os.constants.signals (aliases
   after the canonical name so that the first match is the name Node
   reports) */
static const QBConst qb_signal_table[] = {
#ifdef SIGHUP
    {"SIGHUP", SIGHUP},
#endif
#ifdef SIGINT
    {"SIGINT", SIGINT},
#endif
#ifdef SIGQUIT
    {"SIGQUIT", SIGQUIT},
#endif
#ifdef SIGILL
    {"SIGILL", SIGILL},
#endif
#ifdef SIGTRAP
    {"SIGTRAP", SIGTRAP},
#endif
#ifdef SIGABRT
    {"SIGABRT", SIGABRT},
#endif
#ifdef SIGIOT
    {"SIGIOT", SIGIOT},
#endif
#ifdef SIGBUS
    {"SIGBUS", SIGBUS},
#endif
#ifdef SIGFPE
    {"SIGFPE", SIGFPE},
#endif
#ifdef SIGKILL
    {"SIGKILL", SIGKILL},
#endif
#ifdef SIGUSR1
    {"SIGUSR1", SIGUSR1},
#endif
#ifdef SIGSEGV
    {"SIGSEGV", SIGSEGV},
#endif
#ifdef SIGUSR2
    {"SIGUSR2", SIGUSR2},
#endif
#ifdef SIGPIPE
    {"SIGPIPE", SIGPIPE},
#endif
#ifdef SIGALRM
    {"SIGALRM", SIGALRM},
#endif
#ifdef SIGTERM
    {"SIGTERM", SIGTERM},
#endif
#ifdef SIGCHLD
    {"SIGCHLD", SIGCHLD},
#endif
#ifdef SIGSTKFLT
    {"SIGSTKFLT", SIGSTKFLT},
#endif
#ifdef SIGCONT
    {"SIGCONT", SIGCONT},
#endif
#ifdef SIGSTOP
    {"SIGSTOP", SIGSTOP},
#endif
#ifdef SIGTSTP
    {"SIGTSTP", SIGTSTP},
#endif
#ifdef SIGBREAK
    {"SIGBREAK", SIGBREAK},
#endif
#ifdef SIGTTIN
    {"SIGTTIN", SIGTTIN},
#endif
#ifdef SIGTTOU
    {"SIGTTOU", SIGTTOU},
#endif
#ifdef SIGURG
    {"SIGURG", SIGURG},
#endif
#ifdef SIGXCPU
    {"SIGXCPU", SIGXCPU},
#endif
#ifdef SIGXFSZ
    {"SIGXFSZ", SIGXFSZ},
#endif
#ifdef SIGVTALRM
    {"SIGVTALRM", SIGVTALRM},
#endif
#ifdef SIGPROF
    {"SIGPROF", SIGPROF},
#endif
#ifdef SIGWINCH
    {"SIGWINCH", SIGWINCH},
#endif
#ifdef SIGIO
    {"SIGIO", SIGIO},
#endif
#ifdef SIGPOLL
    {"SIGPOLL", SIGPOLL},
#endif
#ifdef SIGLOST
    {"SIGLOST", SIGLOST},
#endif
#ifdef SIGPWR
    {"SIGPWR", SIGPWR},
#endif
#ifdef SIGINFO
    {"SIGINFO", SIGINFO},
#endif
#ifdef SIGSYS
    {"SIGSYS", SIGSYS},
#endif
#ifdef SIGUNUSED
    {"SIGUNUSED", SIGUNUSED},
#endif
};

/* the name of a signal number ("SIGTERM"), NULL if unknown */
const char *qb_signal_name(int signum)
{
    size_t i;
    for (i = 0; i < countof(qb_signal_table); i++) {
        if (qb_signal_table[i].num == signum)
            return qb_signal_table[i].name;
    }
    return NULL;
}

/* a new object { SIGHUP: 1, ... } (os.constants.signals) */
JSValue qb_signals_object(JSContext *ctx)
{
    return qb_table_object(ctx, qb_signal_table, countof(qb_signal_table));
}

/* os.constants.errno: the positive errno values of the system */
static const QBConst qb_errno_table[] = {
#ifdef E2BIG
    {"E2BIG", E2BIG},
#endif
#ifdef EACCES
    {"EACCES", EACCES},
#endif
#ifdef EADDRINUSE
    {"EADDRINUSE", EADDRINUSE},
#endif
#ifdef EADDRNOTAVAIL
    {"EADDRNOTAVAIL", EADDRNOTAVAIL},
#endif
#ifdef EAFNOSUPPORT
    {"EAFNOSUPPORT", EAFNOSUPPORT},
#endif
#ifdef EAGAIN
    {"EAGAIN", EAGAIN},
#endif
#ifdef EALREADY
    {"EALREADY", EALREADY},
#endif
#ifdef EBADF
    {"EBADF", EBADF},
#endif
#ifdef EBADMSG
    {"EBADMSG", EBADMSG},
#endif
#ifdef EBUSY
    {"EBUSY", EBUSY},
#endif
#ifdef ECANCELED
    {"ECANCELED", ECANCELED},
#endif
#ifdef ECHILD
    {"ECHILD", ECHILD},
#endif
#ifdef ECONNABORTED
    {"ECONNABORTED", ECONNABORTED},
#endif
#ifdef ECONNREFUSED
    {"ECONNREFUSED", ECONNREFUSED},
#endif
#ifdef ECONNRESET
    {"ECONNRESET", ECONNRESET},
#endif
#ifdef EDEADLK
    {"EDEADLK", EDEADLK},
#endif
#ifdef EDESTADDRREQ
    {"EDESTADDRREQ", EDESTADDRREQ},
#endif
#ifdef EDOM
    {"EDOM", EDOM},
#endif
#ifdef EDQUOT
    {"EDQUOT", EDQUOT},
#endif
#ifdef EEXIST
    {"EEXIST", EEXIST},
#endif
#ifdef EFAULT
    {"EFAULT", EFAULT},
#endif
#ifdef EFBIG
    {"EFBIG", EFBIG},
#endif
#ifdef EHOSTUNREACH
    {"EHOSTUNREACH", EHOSTUNREACH},
#endif
#ifdef EIDRM
    {"EIDRM", EIDRM},
#endif
#ifdef EILSEQ
    {"EILSEQ", EILSEQ},
#endif
#ifdef EINPROGRESS
    {"EINPROGRESS", EINPROGRESS},
#endif
#ifdef EINTR
    {"EINTR", EINTR},
#endif
#ifdef EINVAL
    {"EINVAL", EINVAL},
#endif
#ifdef EIO
    {"EIO", EIO},
#endif
#ifdef EISCONN
    {"EISCONN", EISCONN},
#endif
#ifdef EISDIR
    {"EISDIR", EISDIR},
#endif
#ifdef ELOOP
    {"ELOOP", ELOOP},
#endif
#ifdef EMFILE
    {"EMFILE", EMFILE},
#endif
#ifdef EMLINK
    {"EMLINK", EMLINK},
#endif
#ifdef EMSGSIZE
    {"EMSGSIZE", EMSGSIZE},
#endif
#ifdef EMULTIHOP
    {"EMULTIHOP", EMULTIHOP},
#endif
#ifdef ENAMETOOLONG
    {"ENAMETOOLONG", ENAMETOOLONG},
#endif
#ifdef ENETDOWN
    {"ENETDOWN", ENETDOWN},
#endif
#ifdef ENETRESET
    {"ENETRESET", ENETRESET},
#endif
#ifdef ENETUNREACH
    {"ENETUNREACH", ENETUNREACH},
#endif
#ifdef ENFILE
    {"ENFILE", ENFILE},
#endif
#ifdef ENOBUFS
    {"ENOBUFS", ENOBUFS},
#endif
#ifdef ENODATA
    {"ENODATA", ENODATA},
#endif
#ifdef ENODEV
    {"ENODEV", ENODEV},
#endif
#ifdef ENOENT
    {"ENOENT", ENOENT},
#endif
#ifdef ENOEXEC
    {"ENOEXEC", ENOEXEC},
#endif
#ifdef ENOLCK
    {"ENOLCK", ENOLCK},
#endif
#ifdef ENOLINK
    {"ENOLINK", ENOLINK},
#endif
#ifdef ENOMEM
    {"ENOMEM", ENOMEM},
#endif
#ifdef ENOMSG
    {"ENOMSG", ENOMSG},
#endif
#ifdef ENOPROTOOPT
    {"ENOPROTOOPT", ENOPROTOOPT},
#endif
#ifdef ENOSPC
    {"ENOSPC", ENOSPC},
#endif
#ifdef ENOSR
    {"ENOSR", ENOSR},
#endif
#ifdef ENOSTR
    {"ENOSTR", ENOSTR},
#endif
#ifdef ENOSYS
    {"ENOSYS", ENOSYS},
#endif
#ifdef ENOTCONN
    {"ENOTCONN", ENOTCONN},
#endif
#ifdef ENOTDIR
    {"ENOTDIR", ENOTDIR},
#endif
#ifdef ENOTEMPTY
    {"ENOTEMPTY", ENOTEMPTY},
#endif
#ifdef ENOTSOCK
    {"ENOTSOCK", ENOTSOCK},
#endif
#ifdef ENOTSUP
    {"ENOTSUP", ENOTSUP},
#endif
#ifdef ENOTTY
    {"ENOTTY", ENOTTY},
#endif
#ifdef ENXIO
    {"ENXIO", ENXIO},
#endif
#ifdef EOPNOTSUPP
    {"EOPNOTSUPP", EOPNOTSUPP},
#endif
#ifdef EOVERFLOW
    {"EOVERFLOW", EOVERFLOW},
#endif
#ifdef EPERM
    {"EPERM", EPERM},
#endif
#ifdef EPIPE
    {"EPIPE", EPIPE},
#endif
#ifdef EPROTO
    {"EPROTO", EPROTO},
#endif
#ifdef EPROTONOSUPPORT
    {"EPROTONOSUPPORT", EPROTONOSUPPORT},
#endif
#ifdef EPROTOTYPE
    {"EPROTOTYPE", EPROTOTYPE},
#endif
#ifdef ERANGE
    {"ERANGE", ERANGE},
#endif
#ifdef EROFS
    {"EROFS", EROFS},
#endif
#ifdef ESPIPE
    {"ESPIPE", ESPIPE},
#endif
#ifdef ESRCH
    {"ESRCH", ESRCH},
#endif
#ifdef ESTALE
    {"ESTALE", ESTALE},
#endif
#ifdef ETIME
    {"ETIME", ETIME},
#endif
#ifdef ETIMEDOUT
    {"ETIMEDOUT", ETIMEDOUT},
#endif
#ifdef ETXTBSY
    {"ETXTBSY", ETXTBSY},
#endif
#ifdef EWOULDBLOCK
    {"EWOULDBLOCK", EWOULDBLOCK},
#endif
#ifdef EXDEV
    {"EXDEV", EXDEV},
#endif
};

static const QBConst qb_dlopen_table[] = {
#ifdef RTLD_LAZY
    {"RTLD_LAZY", RTLD_LAZY},
#endif
#ifdef RTLD_NOW
    {"RTLD_NOW", RTLD_NOW},
#endif
#ifdef RTLD_GLOBAL
    {"RTLD_GLOBAL", RTLD_GLOBAL},
#endif
#ifdef RTLD_LOCAL
    {"RTLD_LOCAL", RTLD_LOCAL},
#endif
#ifdef RTLD_DEEPBIND
    {"RTLD_DEEPBIND", RTLD_DEEPBIND},
#endif
};

static const QBConst qb_priority_table[] = {
    {"PRIORITY_LOW", UV_PRIORITY_LOW},       {"PRIORITY_BELOW_NORMAL", UV_PRIORITY_BELOW_NORMAL},
    {"PRIORITY_NORMAL", UV_PRIORITY_NORMAL}, {"PRIORITY_ABOVE_NORMAL", UV_PRIORITY_ABOVE_NORMAL},
    {"PRIORITY_HIGH", UV_PRIORITY_HIGH},     {"PRIORITY_HIGHEST", UV_PRIORITY_HIGHEST},
};

#define QB_TABLE_OBJECT(ctx, table) qb_table_object(ctx, table, countof(table))

/* --------------------------------------------------------------- strings */

typedef int QBStringFunc(char *buf, size_t *size);

/* the string returned by a libuv "buffer + size" getter; throws a system
   error named 'syscall' */
static JSValue qb_os_string(JSContext *ctx, QBStringFunc *fn, const char *syscall)
{
    char stack[1024];
    char *buf = stack;
    size_t size = sizeof(stack);
    JSValue ret;
    int err;

    err = fn(buf, &size);
    if (err == UV_ENOBUFS) {
        buf = malloc(size + 1);
        if (!buf)
            return JS_ThrowOutOfMemory(ctx);
        size += 1;
        err = fn(buf, &size);
    }
    if (err < 0)
        ret = qb_throw_uv(ctx, err, syscall, NULL, NULL);
    else
        ret = JS_NewStringLen(ctx, buf, size);
    if (buf != stack)
        free(buf);
    return ret;
}

static JSValue qb_os_hostname(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    return qb_os_string(ctx, uv_os_gethostname, "uv_os_gethostname");
}

static JSValue qb_os_homedir(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    return qb_os_string(ctx, uv_os_homedir, "uv_os_homedir");
}

static JSValue qb_os_tmpdir(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    return qb_os_string(ctx, uv_os_tmpdir, "uv_os_tmpdir");
}

/* ------------------------------------------------------------ resources */

static JSValue qb_os_uptime(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    double up;
    int err = uv_uptime(&up);
    if (err < 0)
        return qb_throw_uv(ctx, err, "uv_uptime", NULL, NULL);
    return JS_NewFloat64(ctx, up);
}

static JSValue qb_os_loadavg(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    double avg[3];
    JSValue v[3];
    uv_loadavg(avg);
    v[0] = JS_NewFloat64(ctx, avg[0]);
    v[1] = JS_NewFloat64(ctx, avg[1]);
    v[2] = JS_NewFloat64(ctx, avg[2]);
    return JS_NewArrayFrom(ctx, 3, v);
}

static JSValue qb_os_totalmem(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    return JS_NewFloat64(ctx, (double)uv_get_total_memory());
}

static JSValue qb_os_freemem(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    /* Node >= 22 reports the available memory */
    return JS_NewFloat64(ctx, (double)uv_get_available_memory());
}

static JSValue qb_os_availableParallelism(JSContext *ctx, JSValueConst this_val, int argc,
                                          JSValueConst *argv)
{
    return JS_NewUint32(ctx, uv_available_parallelism());
}

static JSValue qb_os_cpus(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    uv_cpu_info_t *infos;
    int count, i, err;
    JSValue arr;

    err = uv_cpu_info(&infos, &count);
    if (err < 0)
        return JS_NewArray(ctx); /* like Node: an empty list */
    arr = JS_NewArray(ctx);
    if (JS_IsException(arr))
        goto done;
    for (i = 0; i < count; i++) {
        JSValue cpu, times;
        cpu = JS_NewObject(ctx);
        if (JS_IsException(cpu))
            goto fail;
        times = JS_NewObject(ctx);
        if (JS_IsException(times)) {
            JS_FreeValue(ctx, cpu);
            goto fail;
        }
        if (qb_set(ctx, cpu, "model", JS_NewString(ctx, infos[i].model ? infos[i].model : "")) <
                0 ||
            qb_set(ctx, cpu, "speed", JS_NewInt32(ctx, infos[i].speed)) < 0 ||
            qb_set(ctx, times, "user", JS_NewFloat64(ctx, (double)infos[i].cpu_times.user)) < 0 ||
            qb_set(ctx, times, "nice", JS_NewFloat64(ctx, (double)infos[i].cpu_times.nice)) < 0 ||
            qb_set(ctx, times, "sys", JS_NewFloat64(ctx, (double)infos[i].cpu_times.sys)) < 0 ||
            qb_set(ctx, times, "idle", JS_NewFloat64(ctx, (double)infos[i].cpu_times.idle)) < 0 ||
            qb_set(ctx, times, "irq", JS_NewFloat64(ctx, (double)infos[i].cpu_times.irq)) < 0) {
            JS_FreeValue(ctx, times);
            JS_FreeValue(ctx, cpu);
            goto fail;
        }
        if (qb_set(ctx, cpu, "times", times) < 0) {
            JS_FreeValue(ctx, cpu);
            goto fail;
        }
        if (JS_SetPropertyUint32(ctx, arr, i, cpu) < 0)
            goto fail;
    }
done:
    uv_free_cpu_info(infos, count);
    return arr;
fail:
    JS_FreeValue(ctx, arr);
    uv_free_cpu_info(infos, count);
    return JS_EXCEPTION;
}

/* the prefix length of a netmask, -1 if not contiguous */
static int qb_prefix_length(const uint8_t *mask, int len)
{
    int i, bits = 0;
    for (i = 0; i < len; i++) {
        uint8_t b = mask[i];
        if (b == 0xff) {
            bits += 8;
            continue;
        }
        while (b & 0x80) {
            bits++;
            b = (uint8_t)(b << 1);
        }
        if (b)
            return -1;
        for (i++; i < len; i++) {
            if (mask[i])
                return -1;
        }
        break;
    }
    return bits;
}

static JSValue qb_os_networkInterfaces(JSContext *ctx, JSValueConst this_val, int argc,
                                       JSValueConst *argv)
{
    uv_interface_address_t *addrs;
    int count, i, err;
    JSValue result;

    err = uv_interface_addresses(&addrs, &count);
    if (err < 0)
        return qb_throw_uv(ctx, err, "uv_interface_addresses", NULL, NULL);
    result = JS_NewObject(ctx);
    if (JS_IsException(result))
        goto done;
    for (i = 0; i < count; i++) {
        uv_interface_address_t *ia = &addrs[i];
        char ip[INET6_ADDRSTRLEN + 1], mask[INET6_ADDRSTRLEN + 1];
        char mac[18], cidr[INET6_ADDRSTRLEN + 8];
        const uint8_t *mb;
        int ipv6, prefix;
        int64_t len;
        JSValue entry, list;

        if (ia->address.address4.sin_family == AF_INET) {
            ipv6 = 0;
            uv_ip4_name(&ia->address.address4, ip, sizeof(ip));
            uv_ip4_name(&ia->netmask.netmask4, mask, sizeof(mask));
            mb = (const uint8_t *)&ia->netmask.netmask4.sin_addr;
            prefix = qb_prefix_length(mb, 4);
        } else if (ia->address.address4.sin_family == AF_INET6) {
            ipv6 = 1;
            uv_ip6_name(&ia->address.address6, ip, sizeof(ip));
            uv_ip6_name(&ia->netmask.netmask6, mask, sizeof(mask));
            mb = (const uint8_t *)&ia->netmask.netmask6.sin6_addr;
            prefix = qb_prefix_length(mb, 16);
        } else {
            continue;
        }
        snprintf(mac, sizeof(mac), "%02x:%02x:%02x:%02x:%02x:%02x", (unsigned char)ia->phys_addr[0],
                 (unsigned char)ia->phys_addr[1], (unsigned char)ia->phys_addr[2],
                 (unsigned char)ia->phys_addr[3], (unsigned char)ia->phys_addr[4],
                 (unsigned char)ia->phys_addr[5]);
        entry = JS_NewObject(ctx);
        if (JS_IsException(entry))
            goto fail;
        if (qb_set(ctx, entry, "address", JS_NewString(ctx, ip)) < 0 ||
            qb_set(ctx, entry, "netmask", JS_NewString(ctx, mask)) < 0 ||
            qb_set(ctx, entry, "family", JS_NewString(ctx, ipv6 ? "IPv6" : "IPv4")) < 0 ||
            qb_set(ctx, entry, "mac", JS_NewString(ctx, mac)) < 0 ||
            qb_set(ctx, entry, "internal", JS_NewBool(ctx, ia->is_internal)) < 0)
            goto fail_entry;
        if (prefix >= 0) {
            snprintf(cidr, sizeof(cidr), "%s/%d", ip, prefix);
            if (qb_set(ctx, entry, "cidr", JS_NewString(ctx, cidr)) < 0)
                goto fail_entry;
        } else {
            if (qb_set(ctx, entry, "cidr", JS_NULL) < 0)
                goto fail_entry;
        }
        if (ipv6 && qb_set(ctx, entry, "scopeid",
                           JS_NewUint32(ctx, ia->address.address6.sin6_scope_id)) < 0)
            goto fail_entry;

        list = JS_GetPropertyStr(ctx, result, ia->name);
        if (JS_IsException(list))
            goto fail_entry;
        if (JS_IsUndefined(list)) {
            list = JS_NewArray(ctx);
            if (JS_IsException(list))
                goto fail_entry;
            if (qb_set(ctx, result, ia->name, JS_DupValue(ctx, list)) < 0) {
                JS_FreeValue(ctx, list);
                goto fail_entry;
            }
        }
        if (JS_GetLength(ctx, list, &len) < 0) {
            JS_FreeValue(ctx, list);
            goto fail_entry;
        }
        if (JS_SetPropertyUint32(ctx, list, (uint32_t)len, entry) < 0) {
            JS_FreeValue(ctx, list);
            goto fail;
        }
        JS_FreeValue(ctx, list);
        continue;
    fail_entry:
        JS_FreeValue(ctx, entry);
        goto fail;
    }
done:
    uv_free_interface_addresses(addrs, count);
    return result;
fail:
    JS_FreeValue(ctx, result);
    uv_free_interface_addresses(addrs, count);
    return JS_EXCEPTION;
}

static JSValue qb_os_uname(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    uv_utsname_t u;
    JSValue obj;
    int err = uv_os_uname(&u);
    if (err < 0)
        return qb_throw_uv(ctx, err, "uv_os_uname", NULL, NULL);
    obj = JS_NewObject(ctx);
    if (JS_IsException(obj))
        return obj;
    if (qb_set(ctx, obj, "sysname", JS_NewString(ctx, u.sysname)) < 0 ||
        qb_set(ctx, obj, "release", JS_NewString(ctx, u.release)) < 0 ||
        qb_set(ctx, obj, "version", JS_NewString(ctx, u.version)) < 0 ||
        qb_set(ctx, obj, "machine", JS_NewString(ctx, u.machine)) < 0) {
        JS_FreeValue(ctx, obj);
        return JS_EXCEPTION;
    }
    return obj;
}

/* userInfo(uid?): the passwd entry of the current user (or of 'uid') */
static JSValue qb_os_userInfo(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    uv_passwd_t pw;
    JSValue obj;
    int err;

    if (argc > 0 && JS_IsNumber(argv[0])) {
        int64_t uid;
        if (JS_ToInt64(ctx, &uid, argv[0]))
            return JS_EXCEPTION;
        err = uv_os_get_passwd2(&pw, (uv_uid_t)uid);
    } else {
        err = uv_os_get_passwd(&pw);
    }
    if (err < 0)
        return qb_throw_uv(ctx, err, "uv_os_get_passwd", NULL, NULL);
    obj = JS_NewObject(ctx);
    if (JS_IsException(obj))
        goto done;
    if (qb_set(ctx, obj, "uid", JS_NewInt64(ctx, (int64_t)(long)pw.uid)) < 0 ||
        qb_set(ctx, obj, "gid", JS_NewInt64(ctx, (int64_t)(long)pw.gid)) < 0 ||
        qb_set(ctx, obj, "username", JS_NewString(ctx, pw.username ? pw.username : "")) < 0 ||
        qb_set(ctx, obj, "homedir", JS_NewString(ctx, pw.homedir ? pw.homedir : "")) < 0 ||
        qb_set(ctx, obj, "shell", pw.shell ? JS_NewString(ctx, pw.shell) : JS_NULL) < 0) {
        JS_FreeValue(ctx, obj);
        obj = JS_EXCEPTION;
    }
done:
    uv_os_free_passwd(&pw);
    return obj;
}

static JSValue qb_os_getPriority(JSContext *ctx, JSValueConst this_val, int argc,
                                 JSValueConst *argv)
{
    int32_t pid = 0;
    int prio, err;
    if (argc > 0 && !JS_IsUndefined(argv[0]) && JS_ToInt32(ctx, &pid, argv[0]))
        return JS_EXCEPTION;
    err = uv_os_getpriority((uv_pid_t)pid, &prio);
    if (err < 0)
        return qb_throw_uv(ctx, err, "uv_os_getpriority", NULL, NULL);
    return JS_NewInt32(ctx, prio);
}

static JSValue qb_os_setPriority(JSContext *ctx, JSValueConst this_val, int argc,
                                 JSValueConst *argv)
{
    int32_t pid, prio;
    int err;
    if (JS_ToInt32(ctx, &pid, argv[0]) || JS_ToInt32(ctx, &prio, argv[1]))
        return JS_EXCEPTION;
    err = uv_os_setpriority((uv_pid_t)pid, prio);
    if (err < 0)
        return qb_throw_uv(ctx, err, "uv_os_setpriority", NULL, NULL);
    return JS_UNDEFINED;
}

static JSValue qb_os_constants(JSContext *ctx)
{
    JSValue obj = JS_NewObject(ctx);
    if (JS_IsException(obj))
        return obj;
    if (qb_set(ctx, obj, "signals", qb_signals_object(ctx)) < 0 ||
        qb_set(ctx, obj, "errno", QB_TABLE_OBJECT(ctx, qb_errno_table)) < 0 ||
        qb_set(ctx, obj, "priority", QB_TABLE_OBJECT(ctx, qb_priority_table)) < 0 ||
        qb_set(ctx, obj, "dlopen", QB_TABLE_OBJECT(ctx, qb_dlopen_table)) < 0) {
        JS_FreeValue(ctx, obj);
        return JS_EXCEPTION;
    }
    return obj;
}

static const JSCFunctionListEntry qb_os_funcs[] = {
    JS_CFUNC_DEF("hostname", 0, qb_os_hostname),
    JS_CFUNC_DEF("homedir", 0, qb_os_homedir),
    JS_CFUNC_DEF("tmpdir", 0, qb_os_tmpdir),
    JS_CFUNC_DEF("uptime", 0, qb_os_uptime),
    JS_CFUNC_DEF("loadavg", 0, qb_os_loadavg),
    JS_CFUNC_DEF("totalmem", 0, qb_os_totalmem),
    JS_CFUNC_DEF("freemem", 0, qb_os_freemem),
    JS_CFUNC_DEF("availableParallelism", 0, qb_os_availableParallelism),
    JS_CFUNC_DEF("cpus", 0, qb_os_cpus),
    JS_CFUNC_DEF("networkInterfaces", 0, qb_os_networkInterfaces),
    JS_CFUNC_DEF("uname", 0, qb_os_uname),
    JS_CFUNC_DEF("userInfo", 0, qb_os_userInfo),
    JS_CFUNC_DEF("getPriority", 1, qb_os_getPriority),
    JS_CFUNC_DEF("setPriority", 2, qb_os_setPriority),
};

static int qb_os_module_init(JSContext *ctx, JSModuleDef *m)
{
    JSValue constants;
    if (JS_SetModuleExportList(ctx, m, qb_os_funcs, countof(qb_os_funcs)) < 0)
        return -1;
    constants = qb_os_constants(ctx);
    if (JS_IsException(constants))
        return -1;
    return JS_SetModuleExport(ctx, m, "constants", constants);
}

JSModuleDef *qb_init_os(JSContext *ctx, const char *name)
{
    JSModuleDef *m = JS_NewCModule(ctx, name, qb_os_module_init);
    if (!m)
        return NULL;
    if (JS_AddModuleExportList(ctx, m, qb_os_funcs, countof(qb_os_funcs)) < 0 ||
        JS_AddModuleExport(ctx, m, "constants") < 0)
        return NULL;
    return m;
}
