/*
 * dquickts C API compat shim (plan D5; capi-design.md §3 and §10.2): the
 * subset of CPython 3.15 Include/internal/pycore_fileutils.h used by the vendored
 * modules (capi/modules).  Unit E2 implements the functions.
 * Owner of this file: unit F.  PSF-2.0 for the declarations copied from
 * CPython.
 */
#ifndef Py_INTERNAL_FILEUTILS_H
#define Py_INTERNAL_FILEUTILS_H
#ifndef Py_BUILD_CORE
#  error "this header requires Py_BUILD_CORE define"
#endif

#include <sys/select.h>             // FD_SETSIZE
#include <sys/stat.h>               // struct stat

/* A routine to check if a file descriptor can be select()-ed. */
#define _PyIsSelectable_fd(FD) ((unsigned int)(FD) < (unsigned int)FD_SETSIZE)

PyAPI_FUNC(int) _Py_dup(int fd);

PyAPI_FUNC(int) _Py_IsValidFD(int fd);

/* POSIX: the C library's struct stat (python/capi_stable.c implements
   the functions; mmap) */
#define _Py_stat_struct stat
PyAPI_FUNC(int) _Py_fstat(
    int fd,
    struct _Py_stat_struct *status);
PyAPI_FUNC(int) _Py_fstat_noraise(
    int fd,
    struct _Py_stat_struct *status);

/* open() with O_CLOEXEC; errno set, no exception, on failure (readline) */
PyAPI_FUNC(int) _Py_open_noraise(
    const char *pathname,
    int flags);

PyAPI_FUNC(int) _Py_set_inheritable(int fd, int inheritable,
                                    int *atomic_flag_works);

#endif /* !Py_INTERNAL_FILEUTILS_H */
