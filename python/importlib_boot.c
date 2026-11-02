/*
 * Import system bootstrap: stdlib location, sys.path and the importlib
 * installation.
 *
 * Derived from CPython 3.15 Python/import.c (init_importlib, bootstrap_imp,
 * init_importlib_external, init_zipimport, _PyImport_InitExternal),
 * Python/pylifecycle.c (init_interp_main, init_import_site),
 * Modules/signalmodule.c (signal_install_handlers), Python/codecs.c
 * (_PyCodec_InitRegistry) and a simplified Modules/getpath.py (PSF-2.0).
 *
 * Layout (getpath): the stdlib is the directory `pylib`.
 *  - QPYHOME (the PYTHONHOME of this interpreter; ignored under -E/-I):
 *    prefix = $QPYHOME, stdlib = $QPYHOME/pylib;
 *  - else the compile-time QP_PYLIB_DIR: stdlib = QP_PYLIB_DIR,
 *    prefix = its parent directory;
 *  - else stdlib = <directory of the real executable>/pylib, prefix = that
 *    directory.
 * exec_prefix, base_prefix and base_exec_prefix equal prefix, except in a
 * virtual environment (pyvenv.cfg next to sys.executable or in its parent
 * directory): prefix and exec_prefix are then the venv directory and
 * _base_executable the real interpreter; platlibdir is "lib".
 * sys.path = PYTHONPATH entries (made absolute), stdlib,
 * stdlib/site-packages; the launcher inserts sys.path[0] afterwards.
 *
 * importlib._bootstrap and importlib._bootstrap_external are not frozen:
 * their sources are compiled at startup (optimize=0 and co_filename
 * "<frozen importlib._bootstrap>" / "<frozen importlib._bootstrap_external>"
 * like CPython's frozen code, so tracebacks and the importlib frame
 * trimming look the same) into the modules _frozen_importlib and
 * _frozen_importlib_external, registered in sys.modules under those names
 * before execution exactly like PyImport_ImportFrozenModule does.
 * _install_external_importers() then finds _frozen_importlib_external in
 * sys.modules.  Afterwards both get __spec__/__loader__/__file__ through
 * _bootstrap_external._fix_up_module (their source file), since no frozen
 * importer can describe them (_imp.is_frozen is always False).
 *
 * Bytecode caches: sys.implementation.cache_tag is "quickpy-315"
 * (sysmodule.c) and marshal serialises code objects as QuickJS bytecode
 * (mod_misc.c), so importlib's SourceLoader reads and writes
 * __pycache__/<name>.quickpy-315.pyc like CPython; sys.dont_write_bytecode
 * is -B / PYTHONDONTWRITEBYTECODE.
 */

#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

/* python/mod_io.c: init_sys_streams + init_set_builtins_open. */
static int qp_io_install_stdio(JSContext *ctx);
static void qp_boot_ignore_signals(void);

typedef struct QPBootPaths {
    char *prefix;       /* js_malloc'd, absolute */
    char *stdlib;       /* js_malloc'd, absolute */
} QPBootPaths;

static char *qp_boot_strdup(JSContext *ctx, const char *s, size_t len)
{
    char *r = js_malloc(ctx, len + 1);
    if (r) {
        memcpy(r, s, len);
        r[len] = '\0';
    }
    return r;
}

/* a + "/" + b (a without trailing '/' handling beyond "/"). */
static char *qp_boot_join(JSContext *ctx, const char *a, const char *b)
{
    size_t la = strlen(a), lb = strlen(b);
    int sep = la > 0 && a[la - 1] != '/';
    char *r = js_malloc(ctx, la + sep + lb + 1);
    if (r) {
        memcpy(r, a, la);
        if (sep)
            r[la] = '/';
        memcpy(r + la + sep, b, lb + 1);
    }
    return r;
}

/* _Py_normpath (posixpath.normpath) in place; p must have room for 2
   bytes ("." for an empty path). */
static void qp_boot_normpath(char *p)
{
    size_t n = strlen(p), i = 0, o, start, len, ncomp = 0;
    int initial = 0, dotdot, last_dotdot;

    if (n == 0) {
        strcpy(p, ".");
        return;
    }
    if (p[0] == '/') {
        initial = 1;
        if (p[1] == '/' && p[2] != '/')
            initial = 2;
    }
    o = initial;
    while (i < n) {
        while (i < n && p[i] == '/')
            i++;
        start = i;
        while (i < n && p[i] != '/')
            i++;
        len = i - start;
        if (len == 0 || (len == 1 && p[start] == '.'))
            continue;
        dotdot = len == 2 && p[start] == '.' && p[start + 1] == '.';
        last_dotdot = ncomp > 0 && o >= (size_t)initial + 2 &&
            p[o - 1] == '.' && p[o - 2] == '.' &&
            (o - 2 == (size_t)initial || p[o - 3] == '/');
        if (!dotdot || (!initial && ncomp == 0) || last_dotdot) {
            if (ncomp > 0)
                p[o++] = '/';
            memmove(p + o, p + start, len);
            o += len;
            ncomp++;
        } else if (ncomp > 0) {
            while (o > (size_t)initial && p[o - 1] != '/')
                o--;
            if (o > (size_t)initial)
                o--;            /* the separator before the popped component */
            ncomp--;
        }
    }
    p[o] = '\0';
    if (o == 0)
        strcpy(p, ".");
}

/* getpath abspath(): _Py_abspath(_Py_normpath(path)).  js_malloc'd. */
static char *qp_boot_abspath(JSContext *ctx, const char *path, size_t len)
{
    char cwd[PATH_MAX], *norm, *r;

    norm = js_malloc(ctx, len + 2);
    if (!norm)
        return NULL;
    memcpy(norm, path, len);
    norm[len] = '\0';
    qp_boot_normpath(norm);
    if (norm[0] == '/')
        return norm;
    if (!getcwd(cwd, sizeof(cwd))) {
        js_free(ctx, norm);
        qp_raise_os_error(ctx, errno, QP_NULL);
        return NULL;
    }
    if (!strcmp(norm, "."))
        r = qp_boot_strdup(ctx, cwd, strlen(cwd));
    else
        r = qp_boot_join(ctx, cwd, norm);
    js_free(ctx, norm);
    return r;
}

#ifndef QP_PYLIB_DIR
/* Directory of the real executable (realpath of sys.executable, else the
   OS's record of the running image), js_malloc'd; NULL without error if
   unknown.  Not used when the stdlib directory is compiled in. */
static char *qp_boot_executable_dir(JSContext *ctx)
{
    const char *exe = QP_STATE(ctx)->config.executable;
    char real[PATH_MAX], *slash;
    int found = 0;

    if (exe && exe[0] == '/' && realpath(exe, real))
        found = 1;
#if defined(__APPLE__)
    if (!found) {
        char buf[PATH_MAX];
        uint32_t size = sizeof(buf);
        if (_NSGetExecutablePath(buf, &size) == 0 && realpath(buf, real))
            found = 1;
    }
#elif defined(__linux__)
    if (!found && realpath("/proc/self/exe", real))
        found = 1;
#endif
    if (!found)
        return NULL;
    slash = strrchr(real, '/');
    if (slash == real)
        slash[1] = '\0';
    else
        *slash = '\0';
    return qp_boot_strdup(ctx, real, strlen(real));
}
#endif

static BOOL qp_boot_isfile(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

static void qp_boot_paths_free(JSContext *ctx, QPBootPaths *p)
{
    js_free(ctx, p->prefix);
    js_free(ctx, p->stdlib);
    p->prefix = p->stdlib = NULL;
}

/* 0 or -1 (exception pending). */
static int qp_boot_compute_paths(JSContext *ctx, QPBootPaths *p)
{
    const QPConfig *cfg = &QP_STATE(ctx)->config;
    const char *home = NULL;
    char *landmark;

    p->prefix = p->stdlib = NULL;
    if (cfg->home && cfg->home[0])
        home = cfg->home;   /* embedder setting (JS_InitPython) wins */
    else if (!cfg->ignore_environment && !cfg->isolated)
        home = getenv("QPYHOME");
    if (home && home[0]) {
        p->prefix = qp_boot_abspath(ctx, home, strlen(home));
        if (!p->prefix)
            goto oom;
        p->stdlib = qp_boot_join(ctx, p->prefix, "pylib");
    } else {
#ifdef QP_PYLIB_DIR
        char *slash;
        p->stdlib = qp_boot_abspath(ctx, QP_PYLIB_DIR, strlen(QP_PYLIB_DIR));
        if (!p->stdlib)
            goto oom;
        slash = strrchr(p->stdlib, '/');
        p->prefix = qp_boot_strdup(ctx, p->stdlib,
                                   slash == p->stdlib ? 1 : (size_t)(slash - p->stdlib));
#else
        char *exe_dir = qp_boot_executable_dir(ctx);
        if (!exe_dir) {
            if (JS_HasException(ctx))
                return -1;
            /* no executable location: getpath's last resort is the
               current directory */
            exe_dir = qp_boot_abspath(ctx, "", 0);
            if (!exe_dir)
                goto oom;
        }
        p->prefix = exe_dir;
        p->stdlib = qp_boot_join(ctx, exe_dir, "pylib");
#endif
    }
    if (!p->prefix || !p->stdlib)
        goto oom;
    /* getpath STDLIB_LANDMARKS check */
    landmark = qp_boot_join(ctx, p->stdlib, "os.py");
    if (!landmark)
        goto oom;
    if (!qp_boot_isfile(landmark))
        qp_sys_write_stderr(ctx, "Could not find platform independent libraries <prefix>\n");
    js_free(ctx, landmark);
    return 0;
 oom:
    /* js_malloc failures and getcwd errors have raised already */
    qp_boot_paths_free(ctx, p);
    return -1;
}

/* File system path -> str (filesystem encoding utf-8, surrogateescape). */
static JSValue qp_boot_fs_str(JSContext *ctx, const char *s)
{
    return qp_str_decode(ctx, s, (Py_ssize_t)strlen(s), "utf-8", "surrogateescape");
}

/* sys.<name> = v (consumes v). */
static int qp_boot_sys_set(JSContext *ctx, const char *name, JSValue v)
{
    JSAtom atom;
    int r;

    if (JS_IsException(v))
        return -1;
    atom = JS_NewAtom(ctx, name);
    if (atom == JS_ATOM_NULL) {
        JS_FreeValue(ctx, v);
        return -1;
    }
    r = qp_dict_set_item_atom(ctx, QP_STATE(ctx)->sys_dict, atom, v);
    JS_FreeAtom(ctx, atom);
    JS_FreeValue(ctx, v);
    return r;
}

static int qp_boot_list_append_path(JSContext *ctx, JSValueConst list, const char *path)
{
    JSValue s = qp_boot_fs_str(ctx, path);
    int r;

    if (JS_IsException(s))
        return -1;
    r = qp_list_append(ctx, list, s);
    JS_FreeValue(ctx, s);
    return r;
}

/* sys.path (getpath module_search_paths without the zip/lib-dynload
   entries this layout does not have). */
static JSValue qp_boot_make_sys_path(JSContext *ctx, const QPBootPaths *p)
{
    const char *pythonpath = QP_STATE(ctx)->config.pythonpath;
    JSValue list = qp_list_new(ctx, 0);
    char *entry;
    int r;

    if (JS_IsException(list))
        return list;
    if (pythonpath && pythonpath[0]) {
        const char *s = pythonpath;
        for (;;) {
            const char *e = strchr(s, ':');
            size_t n = e ? (size_t)(e - s) : strlen(s);
            entry = qp_boot_abspath(ctx, s, n);
            if (!entry)
                goto fail;
            r = qp_boot_list_append_path(ctx, list, entry);
            js_free(ctx, entry);
            if (r < 0)
                goto fail;
            if (!e)
                break;
            s = e + 1;
        }
    }
    if (qp_boot_list_append_path(ctx, list, p->stdlib) < 0)
        goto fail;
    entry = qp_boot_join(ctx, p->stdlib, "site-packages");
    if (!entry)
        goto fail;
    r = qp_boot_list_append_path(ctx, list, entry);
    js_free(ctx, entry);
    if (r < 0)
        goto fail;
    return list;
 fail:
    JS_FreeValue(ctx, list);
    return JS_EXCEPTION;
}

/* getpath's virtual environment detection: the directory holding
   pyvenv.cfg next to sys.executable (bin/) or one level up, js_malloc'd;
   NULL without error when the executable is not in a venv. */
static char *qp_boot_venv_prefix(JSContext *ctx)
{
    const char *exe = QP_STATE(ctx)->config.executable;
    char *dir, *cfg, *slash;
    int i;

    if (!exe || exe[0] != '/')
        return NULL;
    slash = strrchr(exe, '/');
    dir = qp_boot_strdup(ctx, exe, slash == exe ? 1 : (size_t)(slash - exe));
    for (i = 0; dir && i < 2; i++) {
        cfg = qp_boot_join(ctx, dir, "pyvenv.cfg");
        if (!cfg)
            break;
        if (qp_boot_isfile(cfg)) {
            js_free(ctx, cfg);
            return dir;
        }
        js_free(ctx, cfg);
        slash = strrchr(dir, '/');
        if (!slash || slash == dir)
            break;
        *slash = '\0';
    }
    js_free(ctx, dir);
    return NULL;
}

/* The sys attributes computed by getpath (interpreter_update_config).  In
   a virtual environment prefix/exec_prefix are the venv directory (site
   adds its site-packages) and _base_executable is the real interpreter;
   base_prefix stays the installation. */
static int qp_boot_init_sys(JSContext *ctx, const QPBootPaths *p)
{
    char *venv = qp_boot_venv_prefix(ctx);
    const char *prefix = venv ? venv : p->prefix;
    int r;

    if (venv) {
        const char *exe = QP_STATE(ctx)->config.executable;
        char real[PATH_MAX];
        if (realpath(exe, real) &&
            qp_boot_sys_set(ctx, "_base_executable", qp_boot_fs_str(ctx, real)) < 0) {
            js_free(ctx, venv);
            return -1;
        }
    }
    r = qp_boot_sys_set(ctx, "prefix", qp_boot_fs_str(ctx, prefix)) < 0 ||
        qp_boot_sys_set(ctx, "exec_prefix", qp_boot_fs_str(ctx, prefix)) < 0;
    js_free(ctx, venv);
    if (r)
        return -1;
    if (qp_boot_sys_set(ctx, "base_prefix", qp_boot_fs_str(ctx, p->prefix)) < 0 ||
        qp_boot_sys_set(ctx, "base_exec_prefix", qp_boot_fs_str(ctx, p->prefix)) < 0 ||
        qp_boot_sys_set(ctx, "platlibdir", qp_str_from_cstr(ctx, "lib")) < 0 ||
        qp_boot_sys_set(ctx, "_stdlib_dir", qp_boot_fs_str(ctx, p->stdlib)) < 0 ||
        qp_boot_sys_set(ctx, "path", qp_boot_make_sys_path(ctx, p)) < 0 ||
        qp_boot_sys_set(ctx, "dont_write_bytecode",
                        qp_bool_new(QP_STATE(ctx)->config.dont_write_bytecode != 0)) < 0)
        return -1;
    return 0;
}

/* Read a whole file (js_malloc'd, NUL-terminated); OSError on failure. */
static char *qp_boot_read_file(JSContext *ctx, const char *path, size_t *plen)
{
    FILE *f;
    struct stat st;
    char *buf = NULL;
    size_t len = 0;
    int err;

    f = fopen(path, "rb");
    if (!f)
        goto os_error;
    if (fstat(fileno(f), &st) < 0)
        goto os_error;
    len = (size_t)st.st_size;
    buf = js_malloc(ctx, len + 1);
    if (!buf) {
        fclose(f);
        return NULL;
    }
    if (fread(buf, 1, len, f) != len) {
        err = ferror(f) ? errno : EIO;
        js_free(ctx, buf);
        fclose(f);
        errno = err;
        f = NULL;
        goto os_error;
    }
    fclose(f);
    buf[len] = '\0';
    *plen = len;
    return buf;
 os_error:
    err = errno;
    if (f)
        fclose(f);
    {
        JSValue fn = qp_boot_fs_str(ctx, path);
        if (!JS_IsException(fn)) {
            qp_raise_os_error(ctx, err, fn);
            JS_FreeValue(ctx, fn);
        }
    }
    return NULL;
}

/* PyImport_ImportFrozenModule for a module compiled from the stdlib source
   <stdlib>/<relpath>: new module `modname` in sys.modules, code compiled
   with optimize=0 under `co_filename`, executed in the module dict (the
   sys.modules entry is removed again on failure, remove_module). */
static JSValue qp_boot_exec_source(JSContext *ctx, const QPBootPaths *p, const char *relpath,
                                   const char *modname, const char *co_filename)
{
    QPState *st = QP_STATE(ctx);
    JSValue name, mod, bc, code, r;
    char *path, *src;
    size_t len;

    path = qp_boot_join(ctx, p->stdlib, relpath);
    if (!path)
        return JS_EXCEPTION;
    src = qp_boot_read_file(ctx, path, &len);
    js_free(ctx, path);
    if (!src)
        return JS_EXCEPTION;
    name = qp_str_from_cstr(ctx, modname);
    if (JS_IsException(name)) {
        js_free(ctx, src);
        return name;
    }
    mod = qp_module_new(ctx, name);
    if (JS_IsException(mod))
        goto fail_name;
    if (qp_dict_set_item(ctx, st->modules, name, mod) < 0)
        goto fail_mod;
    bc = qp_compile_source(ctx, src, len, co_filename, JS_UNDEFINED, QP_INPUT_FILE, 0, 0, NULL);
    js_free(ctx, src);
    src = NULL;
    if (JS_IsException(bc))
        goto fail_remove;
    code = qp_code_new(ctx, bc);
    JS_FreeValue(ctx, bc);
    if (JS_IsException(code))
        goto fail_remove;
    r = qp_eval_code(ctx, code, qp_module_get_dict(mod), qp_module_get_dict(mod));
    JS_FreeValue(ctx, code);
    if (JS_IsException(r))
        goto fail_remove;
    JS_FreeValue(ctx, r);
    JS_FreeValue(ctx, name);
    return mod;
 fail_remove:
    {
        JSValue exc = qp_exc_fetch(ctx);
        if (qp_dict_del_item(ctx, st->modules, name) < 0)
            JS_FreeValue(ctx, qp_exc_fetch(ctx));
        JS_Throw(ctx, exc);
    }
 fail_mod:
    JS_FreeValue(ctx, mod);
 fail_name:
    JS_FreeValue(ctx, name);
    js_free(ctx, src);
    return JS_EXCEPTION;
}

/* obj.<name>(*args) */
static JSValue qp_boot_call_method(JSContext *ctx, JSValueConst obj, const char *name,
                                   int nargs, JSValueConst *args)
{
    JSAtom atom = JS_NewAtom(ctx, name);
    JSValue r;

    if (atom == JS_ATOM_NULL)
        return JS_EXCEPTION;
    r = qp_call_method(ctx, obj, atom, args, nargs, QP_NULL);
    JS_FreeAtom(ctx, atom);
    return r;
}

/* bootstrap_imp: the _imp module from its definition (create + exec). */
static JSValue qp_boot_imp(JSContext *ctx)
{
    const QPModuleDef *def = qp_inittab_find("_imp", 4);

    if (!def)
        return qp_raise(ctx, QP_EXC(ctx, ImportError), "no built-in module named _imp");
    return qp_module_from_def(ctx, def);
}

/* _bootstrap_external._fix_up_module(module.__dict__, name, <stdlib>/<relpath>) */
static int qp_boot_fix_up_module(JSContext *ctx, const QPBootPaths *p, JSValueConst external,
                                 JSValueConst module, const char *modname, const char *relpath)
{
    JSValue args[3], r;
    char *path;
    int ret = -1;

    path = qp_boot_join(ctx, p->stdlib, relpath);
    if (!path)
        return -1;
    args[0] = JS_DupValue(ctx, qp_module_get_dict(module));
    args[1] = qp_str_from_cstr(ctx, modname);
    args[2] = qp_boot_fs_str(ctx, path);
    js_free(ctx, path);
    if (!JS_IsException(args[1]) && !JS_IsException(args[2])) {
        r = qp_boot_call_method(ctx, external, "_fix_up_module", 3, (JSValueConst *)args);
        if (!JS_IsException(r)) {
            JS_FreeValue(ctx, r);
            ret = 0;
        }
    }
    JS_FreeValue(ctx, args[0]);
    JS_FreeValue(ctx, args[1]);
    JS_FreeValue(ctx, args[2]);
    return ret;
}

/* init_zipimport: sys.path_hooks.insert(0, zipimport.zipimporter); a
   missing zipimport is not an error. */
static int qp_boot_init_zipimport(JSContext *ctx, int verbose)
{
    JSValue path_hooks, mod, zipimporter;
    JSAtom atom;
    int r;

    atom = JS_NewAtom(ctx, "path_hooks");
    if (atom == JS_ATOM_NULL)
        return -1;
    path_hooks = qp_sys_get(ctx, atom);
    JS_FreeAtom(ctx, atom);
    if (JS_IsException(path_hooks))
        return -1;
    if (QP_IS_NULL(path_hooks)) {
        qp_raise_runtime_error(ctx, "lost sys.path_hooks");
        return -1;
    }
    if (verbose)
        qp_sys_write_stderr(ctx, "# installing zipimport hook\n");
    zipimporter = JS_EXCEPTION;
    mod = qp_import_module(ctx, "zipimport");
    if (!JS_IsException(mod)) {
        atom = JS_NewAtom(ctx, "zipimporter");
        if (atom != JS_ATOM_NULL) {
            zipimporter = qp_getattr(ctx, mod, atom);
            JS_FreeAtom(ctx, atom);
        }
        JS_FreeValue(ctx, mod);
    }
    if (JS_IsException(zipimporter)) {
        /* No zipimporter object -- okay */
        JS_FreeValue(ctx, qp_exc_fetch(ctx));
        if (verbose)
            qp_sys_write_stderr(ctx, "# can't import zipimport.zipimporter\n");
        JS_FreeValue(ctx, path_hooks);
        return 0;
    }
    if (qp_list_check(ctx, path_hooks)) {
        r = qp_list_insert(ctx, path_hooks, 0, zipimporter);
    } else {
        qp_raise_system_error(ctx, "bad argument to internal function");
        r = -1;
    }
    JS_FreeValue(ctx, zipimporter);
    JS_FreeValue(ctx, path_hooks);
    if (r < 0)
        return -1;
    if (verbose)
        qp_sys_write_stderr(ctx, "# installed zipimport hook\n");
    return 0;
}

/* PyImport_ImportModule(name), result dropped. */
static int qp_boot_import(JSContext *ctx, const char *name)
{
    JSValue m = qp_import_module(ctx, name);

    if (JS_IsException(m))
        return -1;
    JS_FreeValue(ctx, m);
    return 0;
}

/* stdlib location and the sys attributes of getpath (also re-applied to a
   state restored from the base snapshot, qp_importlib_boot_paths) */
static int qp_boot_paths(JSContext *ctx, QPBootPaths *paths)
{
    if (qp_boot_compute_paths(ctx, paths) < 0)
        return -1;
    if (qp_boot_init_sys(ctx, paths) < 0) {
        qp_boot_paths_free(ctx, paths);
        return -1;
    }
    return 0;
}

static int qp_importlib_boot_paths(JSContext *ctx)
{
    QPBootPaths paths;

    if (qp_boot_paths(ctx, &paths) < 0)
        return -1;
    qp_boot_paths_free(ctx, &paths);
    return 0;
}

/* The part of the boot that does not depend on the process (the content
   of the base snapshot, see qp_write_base_snapshot): importlib, the
   zipimport hook, encodings and _signal. */
static int qp_importlib_boot_core(JSContext *ctx)
{
    QPState *st = QP_STATE(ctx);
    int verbose = st->config.verbose;
    QPBootPaths paths;
    JSValue bootstrap = JS_UNDEFINED, external = JS_UNDEFINED, imp = JS_UNDEFINED, r;
    JSValue args[2];
    int ret = -1;

    if (qp_boot_paths(ctx, &paths) < 0)
        return -1;

    /* init_importlib */
    if (verbose)
        qp_sys_write_stderr(ctx, "import _frozen_importlib # frozen\n");
    bootstrap = qp_boot_exec_source(ctx, &paths, "importlib/_bootstrap.py",
                                    "_frozen_importlib", "<frozen importlib._bootstrap>");
    if (JS_IsException(bootstrap))
        goto done;
    JS_FreeValue(ctx, st->importlib);
    st->importlib = JS_DupValue(ctx, bootstrap);
    if (verbose)
        qp_sys_write_stderr(ctx, "import _imp # builtin\n");
    imp = qp_boot_imp(ctx);
    if (JS_IsException(imp))
        goto done;
    {
        /* _PyImport_SetModuleString("_imp", imp_mod) */
        JSAtom atom = JS_NewAtom(ctx, "_imp");
        int rc = atom == JS_ATOM_NULL ? -1 : qp_dict_set_item_atom(ctx, st->modules, atom, imp);
        JS_FreeAtom(ctx, atom);
        if (rc < 0)
            goto done;
    }
    args[0] = st->sys_module;
    args[1] = imp;
    r = qp_boot_call_method(ctx, bootstrap, "_install", 2, (JSValueConst *)args);
    if (JS_IsException(r))
        goto done;
    JS_FreeValue(ctx, r);

    /* _PyImport_InitExternal: init_importlib_external + init_zipimport */
    external = qp_boot_exec_source(ctx, &paths, "importlib/_bootstrap_external.py",
                                   "_frozen_importlib_external",
                                   "<frozen importlib._bootstrap_external>");
    if (JS_IsException(external))
        goto done;
    r = qp_boot_call_method(ctx, bootstrap, "_install_external_importers", 0, NULL);
    if (JS_IsException(r))
        goto done;
    JS_FreeValue(ctx, r);
#ifdef CONFIG_PYTHON_CAPI
    /* version-specific CPython extension files raise a clear ImportError
       (python/capi_dynload.c) */
    if (qp_capi_install_ext_reject(ctx) < 0)
        goto done;
#endif
    if (qp_boot_fix_up_module(ctx, &paths, external, bootstrap, "_frozen_importlib",
                              "importlib/_bootstrap.py") < 0 ||
        qp_boot_fix_up_module(ctx, &paths, external, external, "_frozen_importlib_external",
                              "importlib/_bootstrap_external.py") < 0)
        goto done;
    if (qp_boot_init_zipimport(ctx, verbose) < 0)
        goto done;

    /* _PyUnicode_InitEncodings -> _PyCodec_InitRegistry */
    if (qp_boot_import(ctx, "encodings") < 0)
        goto done;

    /* _PySignal_Init -> signal_install_handlers */
    qp_boot_ignore_signals();
    if (qp_boot_import(ctx, "_signal") < 0)
        goto done;
    ret = 0;
 done:
    JS_FreeValue(ctx, bootstrap);
    JS_FreeValue(ctx, external);
    JS_FreeValue(ctx, imp);
    qp_boot_paths_free(ctx, &paths);
    return ret;
}

/* signal_install_handlers: the signals Python ignores */
static void qp_boot_ignore_signals(void)
{
#ifdef SIGPIPE
    signal(SIGPIPE, SIG_IGN);
#endif
#ifdef SIGXFZ
    signal(SIGXFZ, SIG_IGN);
#endif
#ifdef SIGXFSZ
    signal(SIGXFSZ, SIG_IGN);
#endif
}

/* The part of the boot that depends on the process: the standard
   streams, the warnings options and site. */
static int qp_importlib_boot_process(JSContext *ctx)
{
    QPState *st = QP_STATE(ctx);

    /* init_sys_streams + init_set_builtins_open */
    if (qp_io_install_stdio(ctx) < 0)
        return -1;

    /* init_interp_main: initialize warnings (apply sys.warnoptions) */
    if (st->config.nwarnoptions > 0 && qp_boot_import(ctx, "warnings") < 0) {
        int kbint;
        fprintf(stderr, "'import warnings' failed; traceback:\n");
        qp_err_print_uncaught(ctx, &kbint);
    }

    /* init_import_site */
    if (!st->config.no_site && qp_boot_import(ctx, "site") < 0)
        return -1;
    return 0;
}

static int qp_importlib_boot(JSContext *ctx)
{
    if (qp_importlib_boot_core(ctx) < 0)
        return -1;
    return qp_importlib_boot_process(ctx);
}
