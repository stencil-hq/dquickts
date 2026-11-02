/*
 * dquickts C API (plan D5): inittabs of the built-in CPython modules
 * (capi/Setup.qp).
 *
 * The self-contained modules are part of the engine library and are
 * registered by the runtime itself (python/capi_module.c, from
 * qp_state_init): every embedder of libquickjs gets them.
 *
 * The modules that need a system library (zlib, _ssl, _ctypes, ...) are
 * archived in libqpcapi.a.  A program linked with libqpcapi.a and those
 * libraries registers them before it creates its first Python context:
 *
 *     #ifdef CONFIG_PYTHON_CAPI
 *     #include "capi/qpcapi.h"
 *         PyImport_ExtendInittab(_PyQp_ExtInittab);
 *     #endif
 *
 * The entries become built-in modules (sys.builtin_module_names,
 * _imp.create_builtin) through the inittab bridge of python/capi_module.c.
 *
 * Self-contained: it does not need Python.h (the launcher is a JS/quickjs
 * program); when Python.h was included first its declarations are used.
 */
#ifndef QP_CAPI_QPCAPI_H
#define QP_CAPI_QPCAPI_H

#ifdef __cplusplus
extern "C" {
#endif

#ifndef Py_PYTHON_H
/* CPython's declarations (Include/cpython/import.h), ABI-identical. */
struct _object;
struct _inittab {
    const char *name;               /* ASCII encoded string */
    struct _object *(*initfunc)(void);
};
int PyImport_ExtendInittab(struct _inittab *newtab);
#endif

/* The self-contained modules of capi/Setup.qp ({NULL, NULL} terminated;
   generated as $(CAPI_OBJDIR)/capi_core_inittab.c, part of libquickjs).
   Registered by the runtime: launchers do not use it. */
extern struct _inittab _PyQp_CoreInittab[];

/* The modules of capi/Setup.qp that link a system library, enabled at build
   time ({NULL, NULL} terminated; generated as
   $(CAPI_OBJDIR)/capi_ext_inittab.c, part of libqpcapi.a). */
extern struct _inittab _PyQp_ExtInittab[];

#ifdef __cplusplus
}
#endif

#endif /* QP_CAPI_QPCAPI_H */
