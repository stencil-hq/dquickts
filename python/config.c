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
    &qp_moddef__suggestions,
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
    &qp_moddef__ast,
    &qp_moddef__tokenize,
    &qp_moddef__symtable,
    &qp_moddef__types,
    &qp_moddef__sysconfigdata,
    &qp_moddef__testinternalcapi,
    &qp_moddef_js,
};

/* With the C API, CPython's own C modules (capi/Setup.qp: the launcher's
   PyImport_ExtendInittab entries, then the self-contained modules the
   runtime registers) follow the native ones.
   python/capi_module.c (included after this file) defines them. */
#ifdef CONFIG_PYTHON_CAPI
static int qp_capi_inittab_count(void);
static const QPModuleDef *qp_capi_inittab_get(int i);
static const QPModuleDef *qp_capi_inittab_find(const char *name, size_t len);
#endif
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

/* The struct sequence descriptors of the native modules: a type made by
   qp_structseq_new_type() from one of them records its index here
   (python/structseq.c), which does not depend on the layout of the
   executable, so that its snapshots are read by other builds of the same
   sources (the base snapshot of qpy is written by qpy-nobase). */
static const QPStructSeqDesc *const qp_ss_registry[] = {
    &qp_version_info_desc,
    &qp_flags_desc,
    &qp_float_info_desc,
    &qp_int_info_desc,
    &qp_hash_info_desc,
    &qp_thread_info_desc,
    &qp_asyncgen_hooks_desc,
    &qp_unraisable_args_desc,
#ifdef QP_SIGNAL_HAVE_SIGWAITINFO
    &qp_signal_siginfo_desc,
#endif
    &qp_os_stat_result_desc,
    &qp_os_statvfs_result_desc,
    &qp_os_uname_result_desc,
    &qp_os_terminal_size_desc,
    &qp_os_times_result_desc,
    &qp_os_waitid_result_desc,
#ifdef QP_OS_HAVE_SCHED_PARAM
    &qp_os_sched_param_desc,
#endif
    &qp_thread_excepthook_desc,
    &qp_time_struct_time_desc,
};

static int qp_ss_registry_index(const QPStructSeqDesc *desc)
{
    int i;
    for (i = 0; i < (int)countof(qp_ss_registry); i++) {
        if (qp_ss_registry[i] == desc)
            return i;
    }
    return -1;
}

static const QPStructSeqDesc *qp_ss_registry_get(int index)
{
    if (index < 0 || index >= (int)countof(qp_ss_registry))
        return NULL;
    return qp_ss_registry[index];
}
