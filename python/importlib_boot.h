/*
 * Import system bootstrap (python/importlib_boot.c).
 */
#ifndef QP_IMPORTLIB_BOOT_H
#define QP_IMPORTLIB_BOOT_H

/* Defined when python/importlib_boot.c is part of the build; the launcher
   calls qp_importlib_boot only then. */
#define QP_IMPORTLIB_BOOT 1

/* CPython init_interp_main from the import system on: stdlib location and
   sys.prefix/exec_prefix/base_prefix/base_exec_prefix/platlibdir/
   _stdlib_dir/path, importlib._bootstrap (as _frozen_importlib) +
   _install(sys, _imp), importlib._bootstrap_external (as
   _frozen_importlib_external) + _install_external_importers(), the
   zipimport path hook, encodings, _signal, the io standard streams and
   builtins.open, then `site` unless -S.  Called by the launcher after
   qp_state_init and BEFORE it inserts QPConfig.sys_path_0 into sys.path
   (CPython pymain_sys_path_add_path0 runs after Py_Initialize).  0, or -1
   with a Python exception pending (a fatal initialization error). */
static int qp_importlib_boot(JSContext *ctx);

#endif /* QP_IMPORTLIB_BOOT_H */
