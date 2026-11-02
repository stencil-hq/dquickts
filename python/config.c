/*
 * Builtin (native) module table, the counterpart of CPython's
 * Modules/config.c.  Included last by python/quickpy.c so every
 * qp_moddef_<name> definition is visible.  One entry per native module;
 * sys.builtin_module_names lists them in this order.
 */

static const QPModuleDef *const qp_inittab[] = {
    &qp_moddef_builtins,
    &qp_moddef_sys,
    &qp_moddef__string,
    &qp_moddef__typing,
    &qp_moddef_gc,
    &qp_moddef_atexit,
    &qp_moddef__signal,
    &qp_moddef__contextvars,
    &qp_moddef_marshal,
    &qp_moddef__warnings,
    &qp_moddef__weakref,
    &qp_moddef__thread,
    &qp_moddef_time,
    &qp_moddef_errno,
    &qp_moddef__stat,
    &qp_moddef_posix,
    &qp_moddef_select,
    &qp_moddef__posixsubprocess,
    &qp_moddef__codecs,
    &qp_moddef__locale,
    &qp_moddef__io,
    &qp_moddef__imp,
};

/* With the C API, CPython's own C modules (libqpcapi.a, registered by the
   launcher through PyImport_ExtendInittab) follow the native ones. */
static int qp_inittab_count(void)
{
#ifdef CONFIG_PYTHON_CAPI
    return (int)countof(qp_inittab) + qp_capi_inittab_count();
#else
    return (int)countof(qp_inittab);
#endif
}

static const QPModuleDef *qp_inittab_get(int i)
{
#ifdef CONFIG_PYTHON_CAPI
    if (i >= (int)countof(qp_inittab))
        return qp_capi_inittab_get(i - (int)countof(qp_inittab));
#endif
    return qp_inittab[i];
}

static const QPModuleDef *qp_inittab_find(const char *name, size_t len)
{
    int i;
    for (i = 0; i < (int)countof(qp_inittab); i++) {
        const char *n = qp_inittab[i]->name;
        if (strlen(n) == len && !memcmp(n, name, len))
            return qp_inittab[i];
    }
#ifdef CONFIG_PYTHON_CAPI
    return qp_capi_inittab_find(name, len);
#else
    return NULL;
#endif
}
