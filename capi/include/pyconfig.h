/*
 * dquickts C API (D5): configuration header normally produced by CPython's
 * configure.  Hand-written and portable across the supported hosts (64-bit
 * Linux/glibc or musl, macOS); sizes come from compiler predefines.
 *
 * Two users:
 *  - CPython Modules sources compiled as separate translation units against
 *    capi/include (feature-test macros are honoured: this header must come
 *    first, as Python.h requires);
 *  - the quickjs.c translation unit through python/capi.h (QP_CAPI_IMPL):
 *    system headers are already included there, so feature-test macros are
 *    not (re)defined.
 *
 * Owner: unit F (build integration).  HAVE_* entries beyond the base set are
 * added when a vendored module needs them (record the module in a comment).
 */
#ifndef Py_PYCONFIG_H
#define Py_PYCONFIG_H

#if !defined(__LP64__) && !defined(_LP64)
#  error "dquickts C API: 64-bit hosts only"
#endif

/* ---- feature-test macros (extension TUs only) ---------------------------- */
#ifndef QP_CAPI_IMPL
#  ifndef _GNU_SOURCE
#    define _GNU_SOURCE 1
#  endif
#  ifdef __APPLE__
#    ifndef _DARWIN_C_SOURCE
#      define _DARWIN_C_SOURCE 1
#    endif
#  endif
#  ifndef _FILE_OFFSET_BITS
#    define _FILE_OFFSET_BITS 64
#  endif
#  ifndef _LARGEFILE_SOURCE
#    define _LARGEFILE_SOURCE 1
#  endif
#  ifndef _REENTRANT
#    define _REENTRANT 1
#  endif
#endif

/* ---- sizes and representation -------------------------------------------- */
#define SIZEOF_SHORT        __SIZEOF_SHORT__
#define SIZEOF_INT          __SIZEOF_INT__
#define SIZEOF_LONG         __SIZEOF_LONG__
#define SIZEOF_LONG_LONG    __SIZEOF_LONG_LONG__
#define SIZEOF_FLOAT        __SIZEOF_FLOAT__
#define SIZEOF_DOUBLE       __SIZEOF_DOUBLE__
#define SIZEOF_LONG_DOUBLE  __SIZEOF_LONG_DOUBLE__
#define SIZEOF_VOID_P       __SIZEOF_POINTER__
#define SIZEOF_SIZE_T       __SIZEOF_SIZE_T__
#define SIZEOF_UINTPTR_T    __SIZEOF_POINTER__
#define SIZEOF_WCHAR_T      __SIZEOF_WCHAR_T__
#define SIZEOF__BOOL        1
#define SIZEOF_TIME_T       8
#define SIZEOF_OFF_T        8
#define SIZEOF_FPOS_T       8
#define SIZEOF_PID_T        4
#define SIZEOF_PTHREAD_T    8
#define SIZEOF_PTHREAD_KEY_T __SIZEOF_LONG__
#define ALIGNOF_LONG        __SIZEOF_LONG__
#define ALIGNOF_SIZE_T      __SIZEOF_SIZE_T__
#define ALIGNOF_MAX_ALIGN_T 16

#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
#  define WORDS_BIGENDIAN 1
#  define DOUBLE_IS_BIG_ENDIAN_IEEE754 1
#else
#  define DOUBLE_IS_LITTLE_ENDIAN_IEEE754 1
#endif
#ifdef __SIZEOF_INT128__
#  define HAVE___UINT128_T 1
#endif

/* ---- build options ------------------------------------------------------- */
#define WITH_DOC_STRINGS 1
#define HAVE_SSIZE_T 1
#define HAVE_LONG_LONG 1
#define HAVE_THREAD_LOCAL 1
#define HAVE_STD_ATOMIC 1
#define HAVE_BUILTIN_ATOMIC 1
#define HAVE_COMPUTED_GOTOS 1
#define HAVE_DYNAMIC_LOADING 1          /* M5: abi3/abi3t loader */
#define _Py_STACK_GROWS_DOWN 1
#define _Py_HAVE_STRERROR_R 1
#define STDC_HEADERS 1
#define RETSIGTYPE void
/* PyObject_Malloc is the runtime's allocator (python/capi_object.c); the
   pymalloc/mimalloc options of CPython do not apply.  WITH_PYMALLOC,
   WITH_MIMALLOC, WITH_DTRACE, Py_DEBUG, Py_REF_DEBUG, Py_TRACE_REFS,
   Py_STATS and Py_GIL_DISABLED are never defined. */

/* ---- headers and functions present on every supported host -------------- */
#define HAVE_ALLOCA_H 1
#define HAVE_DIRENT_H 1
#define HAVE_DLFCN_H 1
#define HAVE_ERRNO_H 1
#define HAVE_FCNTL_H 1
#define HAVE_GRP_H 1
#define HAVE_LANGINFO_H 1
#define HAVE_NETDB_H 1
#define HAVE_NETINET_IN_H 1
#define HAVE_NETINET_TCP_H 1
#define HAVE_ARPA_INET_H 1
#define HAVE_NET_IF_H 1
#define HAVE_POLL_H 1
#define HAVE_PTHREAD_H 1
#define HAVE_PWD_H 1
#define HAVE_SCHED_H 1
#define HAVE_SIGNAL_H 1
#define HAVE_STDINT_H 1
#define HAVE_STDIO_H 1
#define HAVE_STDLIB_H 1
#define HAVE_STRINGS_H 1
#define HAVE_STRING_H 1
#define HAVE_SYS_FILE_H 1
#define HAVE_SYS_IOCTL_H 1
#define HAVE_SYS_MMAN_H 1
#define HAVE_SYS_PARAM_H 1
#define HAVE_SYS_POLL_H 1
#define HAVE_SYS_RESOURCE_H 1
#define HAVE_SYS_SELECT_H 1
#define HAVE_SYS_SOCKET_H 1
#define HAVE_SYS_STAT_H 1
#define HAVE_SYS_TIMES_H 1
#define HAVE_SYS_TIME_H 1
#define HAVE_SYS_TYPES_H 1
#define HAVE_SYS_UIO_H 1
#define HAVE_SYS_UN_H 1
#define HAVE_SYS_UTSNAME_H 1
#define HAVE_SYS_WAIT_H 1
#define HAVE_TERMIOS_H 1
#define HAVE_UNISTD_H 1
#define HAVE_UTIME_H 1
#define HAVE_WCHAR_H 1
#define SYS_SELECT_WITH_SYS_TIME 1

#define HAVE_ACCEPT 1
#define HAVE_ACOSH 1
#define HAVE_ADDRINFO 1
#define HAVE_ASINH 1
#define HAVE_ATANH 1
#define HAVE_BIND 1
#define HAVE_CLOCK 1
#define HAVE_CLOCK_GETRES 1
#define HAVE_CLOCK_GETTIME 1
#define HAVE_CLOCK_SETTIME 1
#define HAVE_CONNECT 1
#define HAVE_DUP 1
#define HAVE_DUP2 1
#define HAVE_ERF 1
#define HAVE_ERFC 1
#define HAVE_EXPM1 1
#define HAVE_FORK 1
#define HAVE_FSTAT 1
#define HAVE_GAI_STRERROR 1
#define HAVE_GETADDRINFO 1
#define HAVE_GETHOSTBYADDR 1
#define HAVE_GETHOSTBYNAME 1
#define HAVE_GETHOSTNAME 1
#define HAVE_GETNAMEINFO 1
#define HAVE_GETPEERNAME 1
#define HAVE_GETPID 1
#define HAVE_GETPROTOBYNAME 1
#define HAVE_GETSERVBYNAME 1
#define HAVE_GETSERVBYPORT 1
#define HAVE_GETSOCKNAME 1
#define HAVE_GETTIMEOFDAY 1
#define HAVE_GMTIME_R 1
#define HAVE_HSTRERROR 1
#define HAVE_INET_ATON 1
#define HAVE_INET_NTOA 1
#define HAVE_INET_PTON 1
#define HAVE_IF_NAMEINDEX 1
#define HAVE_LISTEN 1
#define HAVE_LOCALTIME_R 1
#define HAVE_LOG1P 1
#define HAVE_LOG2 1
#define HAVE_MKTIME 1
#define HAVE_NANOSLEEP 1
#define HAVE_POLL 1
#define HAVE_RECVFROM 1
#define HAVE_SENDTO 1
#define HAVE_SETSOCKOPT 1
#define HAVE_SHUTDOWN 1
#define HAVE_SIGACTION 1
#define HAVE_SNPRINTF 1
#define HAVE_SOCKADDR_STORAGE 1
#define HAVE_SOCKET 1
#define HAVE_SOCKETPAIR 1
#define HAVE_SOCKLEN_T 1
#define HAVE_STRFTIME 1
#define HAVE_SYSCONF 1
#define HAVE_TIMEGM 1
#define HAVE_TM_ZONE 1
#define HAVE_STRUCT_TM_TM_ZONE 1
#define HAVE_TZNAME 1
#define HAVE_UNAME 1
#define HAVE_WCSCOLL 1
#define HAVE_WCSFTIME 1
#define HAVE_WCSXFRM 1
#define HAVE_WMEMCMP 1
#define ENABLE_IPV6 1

/* ---- host specific -------------------------------------------------------- */
#if defined(__APPLE__)
#  define HAVE_SOCKADDR_SA_LEN 1
#  define HAVE_SYS_EVENT_H 1
#  define HAVE_KQUEUE 1
#  define HAVE_SYS_SYSCTL_H 1
#  define HAVE_SYSCTLBYNAME 1
#  define HAVE_STRUCT_STAT_ST_BIRTHTIME 1
#  define HAVE_STRUCT_STAT_ST_FLAGS 1
#  define HAVE_STAT_TV_NSEC2 1
#  define HAVE_GETENTROPY 1
#  define HAVE_SYS_RANDOM_H 1
#  define HAVE_STRLCPY 1
#  define WITH_DYLD 1
#elif defined(__linux__)
#  define HAVE_LINUX_NETLINK_H 1
#  define HAVE_LINUX_CAN_H 1
#  define HAVE_LINUX_TIPC_H 1
#  define HAVE_SYS_EPOLL_H 1
#  define HAVE_EPOLL 1
#  define HAVE_EPOLL_CREATE1 1
#  define HAVE_SYS_RANDOM_H 1
#  define HAVE_GETRANDOM 1
#  define HAVE_GETRANDOM_SYSCALL 1
#  define HAVE_ACCEPT4 1
#  define HAVE_STAT_TV_NSEC 1
#  define HAVE_SYS_SYSCALL_H 1
#  define HAVE_GETHOSTBYNAME_R 1
#  define HAVE_GETHOSTBYNAME_R_6_ARG 1
#  define HAVE_NETPACKET_PACKET_H 1
#  define HAVE_SOCKADDR_ALG 1
#  define HAVE_LINUX_VM_SOCKETS_H 1
#endif

#endif /* Py_PYCONFIG_H */
