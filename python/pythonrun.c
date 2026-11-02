/*
 * Running Python code as __main__ and the `python` command line (unit l).
 *
 * Derived from CPython Modules/main.c (pymain_*), Python/pythonrun.c
 * (_PyRun_SimpleFile, _PyRun_String, run_mod, _PyRun_InteractiveLoop,
 * _PyRun_InteractiveOne), Python/initconfig.c and Python/getopt.c (command
 * line, environment variables, help texts), Python/pathconfig.c
 * (_PyPathConfig_ComputeSysPath0), Objects/unicodeobject.c
 * (_PyUnicode_Dedent) and Lib/codeop.py (_maybe_compile, used for the
 * interactive incomplete-input detection) (PSF-2.0).
 *
 * Differences with CPython, all outside M2:
 *  - the interactive loop is CPython's basic REPL (PYTHON_BASIC_REPL):
 *    _pyrepl needs the M3 standard library; prompts go to stderr like
 *    PyOS_StdioReadline (no readline module);
 *  - an unhandled KeyboardInterrupt exits with status 130 instead of
 *    re-raising SIGINT (no signal module in M2: SIGINT keeps its default
 *    action, which already terminates the process the CPython way);
 *  - the runtime is not torn down at exit (no module cleanup): threading
 *    shutdown and atexit callbacks run, the standard streams are flushed
 *    and the status returned.
 */

#include <sys/stat.h>
#include <sys/resource.h>
#include <unistd.h>
#include <limits.h>
#include <locale.h>
#include <signal.h>

#include "quickpy.h"
#include "pythonrun_usage.h"

/* provided by sysmodule.c (k) and errors.c (i) */
static int qp_sys_flush_std_files(JSContext *ctx);
static int qp_err_print_uncaught(JSContext *ctx, int *pkbint);
static int qp_linecache_register(JSContext *ctx, const char *filename, const char *src, size_t len);
/* provided by mod_misc.c */
static void (*qp_signal_attach(JSContext *ctx))(int);
static void qp_signal_apply_inherited(JSContext *ctx, const uint8_t *ignored, int nsig);
/* provided by mod_time.c and mod_posix.c */
static int qp_time_init_timezone(JSContext *ctx, JSValueConst m);
static void qp_os_drepl_environ(JSContext *ctx, BOOL fill);

#define QP_PY_VERSION "3.15.2"

/* ---- running code ----------------------------------------------------- */

/* run_eval_code_obj: run a module-level bytecode (consumed) with globals
   and locals (QP_NULL = globals), inserting __builtins__ like CPython. */
static JSValue qp_run_code(JSContext *ctx, JSValue bytecode, JSValueConst globals,
                           JSValueConst locals)
{
    JSValue func, res;
    int rc;

    rc = qp_dict_get_item_atom(ctx, globals, QP_ATOM(ctx, __builtins__), &func);
    if (rc < 0) {
        JS_FreeValue(ctx, bytecode);
        return JS_EXCEPTION;
    }
    if (rc > 0) {
        JS_FreeValue(ctx, func);
    } else if (qp_dict_set_item_atom(ctx, globals, QP_ATOM(ctx, __builtins__),
                                     QP_STATE(ctx)->builtins_module) < 0) {
        JS_FreeValue(ctx, bytecode);
        return JS_EXCEPTION;
    }
    func = qp_function_new(ctx, bytecode, globals);
    JS_FreeValue(ctx, bytecode);
    if (JS_IsException(func))
        return func;
    res = qp_call_with_locals(ctx, func, QP_IS_NULL(locals) ? globals : locals);
    JS_FreeValue(ctx, func);
    qp_run_pending_finalizers(ctx);
    return res;
}

/* Compile src (src[len] == '\0') with the given compile flags and run it. */
static JSValue qp_run_source(JSContext *ctx, const char *src, size_t len, const char *filename,
                             int mode, int flags, JSValueConst globals, JSValueConst locals)
{
    qp_error err;
    JSValue bc, module = QP_NULL;

    /* _PyRun_StringFlagsWithName: SyntaxWarnings are reported for
       globals['__name__'] */
    if (qp_dict_get_item_atom(ctx, globals, QP_ATOM(ctx, __name__), &module) < 0)
        return JS_EXCEPTION;
    memset(&err, 0, sizeof(err));
    bc = qp_compile_source(ctx, src, len, filename, module, mode, flags, -1, &err);
    JS_FreeValue(ctx, module);
    if (JS_IsException(bc)) {
        if (err.kind != QP_ERR_NONE)
            qp_raise_qp_error(ctx, &err, filename);
        qp_error_clear(&err);
        return bc;
    }
    qp_error_clear(&err);
    return qp_run_code(ctx, bc, globals, locals);
}

/* PyRun_StringFlags: src is str text (no coding cookie). */
static JSValue qp_run_string(JSContext *ctx, const char *src, size_t len, const char *filename,
                             int mode, JSValueConst globals, JSValueConst locals)
{
    return qp_run_source(ctx, src, len, filename, mode, QP_PF_IGNORE_COOKIE, globals, locals);
}

/* flush_io: flush sys.stderr and sys.stdout, ignoring errors. */
static void qp_run_flush_io(JSContext *ctx)
{
    static const QPAtomId names[2] = { QP_ATOM_stderr, QP_ATOM_stdout };
    JSValue f, r;
    int i;

    for (i = 0; i < 2; i++) {
        f = qp_sys_get(ctx, QP_STATE(ctx)->atoms[names[i]]);
        if (JS_IsException(f)) {
            JS_FreeValue(ctx, JS_GetException(ctx));
            continue;
        }
        if (QP_IS_NULL(f) || JS_IsUndefined(f))
            continue;
        r = qp_call_method(ctx, f, QP_ATOM(ctx, flush), NULL, 0, QP_NULL);
        if (JS_IsException(r))
            JS_FreeValue(ctx, JS_GetException(ctx));
        JS_FreeValue(ctx, r);
        JS_FreeValue(ctx, f);
    }
}

/* Uncaught exception at top level: SystemExit -> its status; otherwise the
   exception is shown through sys.excepthook and the status is 1;
   KeyboardInterrupt -> 130 (see the header comment). */
static int qp_handle_uncaught(JSContext *ctx)
{
    int kb = 0;
    int code = qp_err_print_uncaught(ctx, &kb);
    return kb ? 130 : code;
}

/* PyImport_AddModuleRef("__main__") + add_main_module: the __main__
   module (created in sys.modules if missing) with __builtins__ and, once
   importlib is booted, __loader__ = BuiltinImporter. */
static JSValue qp_run_main_module(JSContext *ctx)
{
    QPState *s = QP_STATE(ctx);
    JSValue name, m, d, v, loader;
    JSAtom a;
    int rc;

    name = qp_str_from_atom(ctx, QP_ATOM(ctx, __main__));
    if (JS_IsException(name))
        return name;
    rc = qp_dict_get_item(ctx, s->modules, name, &m);
    if (rc < 0)
        goto fail;
    if (rc == 0) {
        m = qp_module_new(ctx, name);
        if (JS_IsException(m))
            goto fail;
        if (qp_dict_set_item(ctx, s->modules, name, m) < 0) {
            JS_FreeValue(ctx, m);
            goto fail;
        }
    }
    JS_FreeValue(ctx, name);
    d = qp_module_get_dict(m);
    if (QP_IS_NULL(d))
        return m;
    rc = qp_dict_get_item_atom(ctx, d, QP_ATOM(ctx, __builtins__), &v);
    if (rc < 0)
        goto fail_m;
    if (rc > 0)
        JS_FreeValue(ctx, v);
    else if (qp_dict_set_item_atom(ctx, d, QP_ATOM(ctx, __builtins__), s->builtins_module) < 0)
        goto fail_m;
    if (JS_IsObject(s->importlib)) {
        rc = qp_dict_get_item_atom(ctx, d, QP_ATOM(ctx, __loader__), &v);
        if (rc < 0)
            goto fail_m;
        if (rc == 0 || JS_IsUndefined(v) || JS_IsNull(v)) {
            a = JS_NewAtom(ctx, "BuiltinImporter");
            loader = qp_getattr(ctx, s->importlib, a);
            JS_FreeAtom(ctx, a);
            if (JS_IsException(loader))
                goto fail_m;
            rc = qp_dict_set_item_atom(ctx, d, QP_ATOM(ctx, __loader__), loader);
            JS_FreeValue(ctx, loader);
            if (rc < 0)
                goto fail_m;
        }
        JS_FreeValue(ctx, v);
    }
    return m;
 fail_m:
    JS_FreeValue(ctx, m);
    return JS_EXCEPTION;
 fail:
    JS_FreeValue(ctx, name);
    return JS_EXCEPTION;
}

/* globals of __main__ (borrowed through the module, which the caller
   keeps alive). */
static JSValue qp_run_main_dict(JSContext *ctx, JSValueConst main_module)
{
    JSValue d = qp_module_get_dict(main_module);
    if (!QP_IS_NULL(d))
        return JS_DupValue(ctx, d);
    return qp_getattr(ctx, main_module, QP_ATOM(ctx, __dict__));
}

/* set_main_loader: importlib._bootstrap_external.SourceFileLoader. */
static int qp_run_set_main_loader(JSContext *ctx, JSValueConst d, JSValueConst filename)
{
    QPState *s = QP_STATE(ctx);
    JSValue ext, loader_type, loader, args[2];
    JSAtom a;
    int rc;

    if (!JS_IsObject(s->importlib))
        return 0;   /* M2: no importlib, __loader__ stays None */
    a = JS_NewAtom(ctx, "_bootstrap_external");
    ext = qp_getattr(ctx, s->importlib, a);
    JS_FreeAtom(ctx, a);
    if (JS_IsException(ext))
        return -1;
    a = JS_NewAtom(ctx, "SourceFileLoader");
    loader_type = qp_getattr(ctx, ext, a);
    JS_FreeAtom(ctx, a);
    JS_FreeValue(ctx, ext);
    if (JS_IsException(loader_type))
        return -1;
    args[0] = qp_str_from_atom(ctx, QP_ATOM(ctx, __main__));
    args[1] = JS_DupValue(ctx, filename);
    loader = JS_IsException(args[0]) ? JS_EXCEPTION
        : qp_vectorcall(ctx, loader_type, (JSValueConst *)args, 2, QP_NULL);
    JS_FreeValue(ctx, args[0]);
    JS_FreeValue(ctx, args[1]);
    JS_FreeValue(ctx, loader_type);
    if (JS_IsException(loader))
        return -1;
    rc = qp_dict_set_item_atom(ctx, d, QP_ATOM(ctx, __loader__), loader);
    JS_FreeValue(ctx, loader);
    return rc;
}

/* _PyRun_SimpleFile: run file contents (bytes, coding cookie honoured) as
   __main__ with __file__ set during the run.  Returns the result or
   JS_EXCEPTION. */
static JSValue qp_run_simple_file(JSContext *ctx, const char *src, size_t len, const char *filename)
{
    JSValue main_module, d, fname, res = JS_EXCEPTION, v;
    int rc, set_file_name = 0;

    main_module = qp_run_main_module(ctx);
    if (JS_IsException(main_module))
        return main_module;
    d = qp_run_main_dict(ctx, main_module);
    if (JS_IsException(d)) {
        JS_FreeValue(ctx, main_module);
        return d;
    }
    fname = qp_str_from_cstr(ctx, filename);
    if (JS_IsException(fname))
        goto done;
    rc = qp_dict_get_item_atom(ctx, d, QP_ATOM(ctx, __file__), &v);
    if (rc < 0)
        goto done;
    if (rc > 0) {
        JS_FreeValue(ctx, v);
    } else {
        if (qp_dict_set_item_atom(ctx, d, QP_ATOM(ctx, __file__), fname) < 0)
            goto done;
        set_file_name = 1;
    }
    /* when running from stdin, leave __main__.__loader__ alone */
    if (strcmp(filename, "<stdin>") != 0 && qp_run_set_main_loader(ctx, d, fname) < 0)
        goto done;
    res = qp_run_source(ctx, src, len, filename, QP_INPUT_FILE, 0, d, d);
    qp_run_flush_io(ctx);
 done:
    if (set_file_name && qp_dict_del_item_atom(ctx, d, QP_ATOM(ctx, __file__)) < 0) {
        fprintf(stderr, "python: failed to delete __main__.__file__\n");
        JS_FreeValue(ctx, res);
        res = JS_EXCEPTION;
    }
    JS_FreeValue(ctx, fname);
    JS_FreeValue(ctx, d);
    JS_FreeValue(ctx, main_module);
    return res;
}

/* ---- command line state ----------------------------------------------- */

typedef struct QPMain {
    QPConfig cfg;
    const char *program;        /* argv[0] (program name in messages) */
    int skip_first_line;        /* -x */
    int kb;                     /* an unhandled KeyboardInterrupt was reported */
    int site_import;            /* !-S */
    char **sys_argv;            /* cfg.argv storage */
    char *executable;           /* cfg.executable storage */
    char *path0;                /* cfg.sys_path_0 storage */
    char *command;              /* -c text + "\n" */
    char *envwarn;              /* PYTHONWARNINGS copy split in place */
    char *run_filename;         /* cfg.run_filename storage (made absolute) */
    int use_frozen_modules;     /* -X frozen_modules / PYTHON_FROZEN_MODULES: start from the base snapshot embedded in qpy (qp_main_restore_base) */
} QPMain;

static int qp_main_err_print(JSContext *ctx, QPMain *m)
{
    int kb = 0;
    int code = qp_err_print_uncaught(ctx, &kb);
    if (kb)
        m->kb = 1;
    return code;
}

/* stdin_is_interactive */
static int qp_main_stdin_is_interactive(const QPMain *m)
{
    return isatty(fileno(stdin)) || m->cfg.interactive;
}

static int qp_main_run_code(const QPMain *m)
{
    return m->cfg.run_command || m->cfg.run_module || m->cfg.run_filename;
}

/* Write a str built with qp_str_from_format to sys.stderr. */
static void qp_main_write_stderr_value(JSContext *ctx, JSValue s)
{
    const char *p;

    if (JS_IsException(s)) {
        JS_FreeValue(ctx, JS_GetException(ctx));
        return;
    }
    p = qp_str_utf8(ctx, s, NULL);
    JS_FreeValue(ctx, s);
    if (!p) {
        JS_FreeValue(ctx, JS_GetException(ctx));
        return;
    }
    qp_sys_write_stderr(ctx, "%s", p);
    JS_FreeCString(ctx, p);
}

/* _PyUnicode_Dedent (spaces and tabs only; whitespace-only lines become
   empty).  Returns a qp_sys_malloc'd copy. */
static char *qp_main_dedent(const char *src, size_t len, size_t *out_len)
{
    const char *end = src + len, *iter, *line_start, *start = NULL, *lws_end, *a, *b;
    size_t ws_len = 0, n;
    char *dest, *d;
    int in_leading;

    for (iter = src; iter < end; ++iter) {
        line_start = iter;
        lws_end = NULL;
        while (iter < end && *iter != '\n') {
            if (!lws_end && *iter != ' ' && *iter != '\t') {
                if (iter == line_start) {
                    ws_len = 0;
                    start = NULL;
                    goto copy;
                }
                lws_end = iter;
            }
            ++iter;
        }
        if (!lws_end)
            continue;
        if (!start) {
            start = line_start;
            ws_len = lws_end - line_start;
        } else {
            n = 0;
            a = start;
            b = line_start;
            while (a < start + ws_len && b < lws_end && *a == *b) {
                a++;
                b++;
                n++;
            }
            ws_len = n;
            if (ws_len == 0)
                break;
        }
    }
 copy:
    dest = qp_sys_malloc(len + 1);
    if (!dest)
        return NULL;
    if (ws_len == 0) {
        memcpy(dest, src, len);
        dest[len] = '\0';
        *out_len = len;
        return dest;
    }
    d = dest;
    for (iter = src; iter < end; ++iter) {
        line_start = iter;
        in_leading = 1;
        while (iter < end && *iter != '\n') {
            if (in_leading && *iter != ' ' && *iter != '\t')
                in_leading = 0;
            ++iter;
        }
        if (in_leading && iter < end) {
            *d++ = '\n';
            continue;
        }
        n = (size_t)(iter - line_start);
        n = n > ws_len ? n - ws_len : 0;
        memcpy(d, line_start + ((size_t)(iter - line_start) > ws_len ? ws_len : (size_t)(iter - line_start)), n);
        d += n;
        if (iter < end)
            *d++ = '\n';
    }
    *d = '\0';
    *out_len = d - dest;
    return dest;
}

/* pymain_run_command */
static int qp_main_run_command(JSContext *ctx, QPMain *m)
{
    JSValue main_module, d, res;
    char *src;
    size_t len;

    src = qp_main_dedent(m->cfg.run_command, strlen(m->cfg.run_command), &len);
    if (!src) {
        qp_sys_write_stderr(ctx, "Unable to decode the command from the command line:\n");
        qp_raise_memory_error(ctx);
        return qp_main_err_print(ctx, m);
    }
    main_module = qp_run_main_module(ctx);
    if (JS_IsException(main_module)) {
        qp_sys_free(src);
        return qp_main_err_print(ctx, m);
    }
    d = qp_run_main_dict(ctx, main_module);
    if (JS_IsException(d)) {
        qp_sys_free(src);
        JS_FreeValue(ctx, main_module);
        return qp_main_err_print(ctx, m);
    }
    /* _PyRun_String with a name: the source is registered for tracebacks */
    if (qp_linecache_register(ctx, "<string>", src, len) < 0)
        JS_FreeValue(ctx, JS_GetException(ctx));
    res = qp_run_source(ctx, src, len, "<string>", QP_INPUT_FILE, QP_PF_IGNORE_COOKIE, d, d);
    qp_sys_free(src);
    JS_FreeValue(ctx, d);
    JS_FreeValue(ctx, main_module);
    if (JS_IsException(res))
        return qp_main_err_print(ctx, m);
    JS_FreeValue(ctx, res);
    return 0;
}

/* Read a whole FILE into a NUL-terminated qp_sys_malloc'd buffer. */
static char *qp_main_read_all(FILE *fp, size_t *plen)
{
    size_t size = 4096, len = 0, n;
    char *buf = qp_sys_malloc(size), *nbuf;

    if (!buf)
        return NULL;
    for (;;) {
        if (size - len < 2) {
            nbuf = qp_sys_realloc(buf, size * 2);
            if (!nbuf) {
                qp_sys_free(buf);
                return NULL;
            }
            buf = nbuf;
            size *= 2;
        }
        n = fread(buf + len, 1, size - len - 1, fp);
        len += n;
        if (n == 0)
            break;
    }
    buf[len] = '\0';
    *plen = len;
    return buf;
}

/* pymain_run_file_obj */
static int qp_main_run_file(JSContext *ctx, QPMain *m)
{
    const char *filename = m->cfg.run_filename;
    struct stat st;
    JSValue fname, res;
    char *src;
    size_t len;
    FILE *fp;
    int ch, err;

    fp = fopen(filename, "rb");
    if (!fp) {
        err = errno;
        fname = qp_str_from_cstr(ctx, filename);
        if (JS_IsException(fname))
            return qp_main_err_print(ctx, m);
        qp_main_write_stderr_value(ctx, qp_str_from_format(ctx, "%s: can't open file %R: [Errno %d] %s\n",
                                                           m->program, fname, err, strerror(err)));
        JS_FreeValue(ctx, fname);
        return 2;
    }
    if (m->skip_first_line) {
        /* push back the first newline so line numbers stay the same */
        while ((ch = getc(fp)) != EOF) {
            if (ch == '\n') {
                ungetc(ch, fp);
                break;
            }
        }
    }
    if (fstat(fileno(fp), &st) == 0 && S_ISDIR(st.st_mode)) {
        fname = qp_str_from_cstr(ctx, filename);
        if (!JS_IsException(fname))
            qp_main_write_stderr_value(ctx, qp_str_from_format(ctx, "%s: %R is a directory, cannot continue\n",
                                                               m->program, fname));
        else
            JS_FreeValue(ctx, JS_GetException(ctx));
        JS_FreeValue(ctx, fname);
        fclose(fp);
        return 1;
    }
    src = qp_main_read_all(fp, &len);
    fclose(fp);
    if (!src) {
        qp_raise_memory_error(ctx);
        return qp_main_err_print(ctx, m);
    }
    res = qp_run_simple_file(ctx, src, len, filename);
    qp_sys_free(src);
    if (JS_IsException(res))
        return qp_main_err_print(ctx, m);
    JS_FreeValue(ctx, res);
    return 0;
}

/* pymain_run_module.  With importlib booted (M3): runpy._run_module_as_main
   (modname, True).  Without it only builtin modules exist, which have no
   code object: runpy's _get_module_details errors are reproduced
   (sys.exit("%s: %s" % (sys.executable, msg)) -> status 1). */
/* -X tracemalloc / PYTHONTRACEMALLOC (init_interp_main):
   _tracemalloc.start(nframe); -1 with the exception pending. */
static int qp_main_start_tracemalloc(JSContext *ctx, int nframe)
{
#ifdef CONFIG_PYTHON_CAPI
    JSValue mod, start, arg, res;
    JSAtom a;

    mod = qp_import_module(ctx, "_tracemalloc");
    if (JS_IsException(mod))
        return -1;
    a = JS_NewAtom(ctx, "start");
    start = qp_getattr(ctx, mod, a);
    JS_FreeAtom(ctx, a);
    JS_FreeValue(ctx, mod);
    if (JS_IsException(start))
        return -1;
    arg = JS_NewInt32(ctx, nframe);
    res = qp_vectorcall(ctx, start, (JSValueConst *)&arg, 1, QP_NULL);
    JS_FreeValue(ctx, start);
    if (JS_IsException(res))
        return -1;
    JS_FreeValue(ctx, res);
#else
    (void)ctx;
    (void)nframe;
#endif
    return 0;
}

static int qp_main_run_module(JSContext *ctx, QPMain *m, const char *modname)
{
    JSValue runpy, runmodule, args[2], res, name, parent, mod;
    const char *exe = m->cfg.executable ? m->cfg.executable : m->program;
    const char *dot;
    size_t len = strlen(modname);
    JSAtom a;

    if (JS_IsObject(QP_STATE(ctx)->importlib)) {
        runpy = qp_import_module(ctx, "runpy");
        if (JS_IsException(runpy)) {
            fprintf(stderr, "Could not import runpy module\n");
            return qp_main_err_print(ctx, m);
        }
        a = JS_NewAtom(ctx, "_run_module_as_main");
        runmodule = qp_getattr(ctx, runpy, a);
        JS_FreeAtom(ctx, a);
        JS_FreeValue(ctx, runpy);
        if (JS_IsException(runmodule)) {
            fprintf(stderr, "Could not access runpy._run_module_as_main\n");
            return qp_main_err_print(ctx, m);
        }
        args[0] = qp_str_from_cstr(ctx, modname);
        if (JS_IsException(args[0])) {
            fprintf(stderr, "Could not convert module name to unicode\n");
            JS_FreeValue(ctx, runmodule);
            return qp_main_err_print(ctx, m);
        }
        args[1] = JS_TRUE;
        res = qp_vectorcall(ctx, runmodule, (JSValueConst *)args, 2, QP_NULL);
        JS_FreeValue(ctx, args[0]);
        JS_FreeValue(ctx, runmodule);
        if (JS_IsException(res))
            return qp_main_err_print(ctx, m);
        JS_FreeValue(ctx, res);
        return 0;
    }

    /* importlib.util.find_spec(mod_name): a dotted name imports its parent */
    dot = strrchr(modname, '.');
    if (dot) {
        parent = qp_str_from_utf8(ctx, modname, dot - modname);
        if (JS_IsException(parent))
            return qp_main_err_print(ctx, m);
        mod = qp_import_module_level(ctx, parent, QP_NULL, QP_NULL, QP_NULL, 0);
        JS_FreeValue(ctx, parent);
        if (JS_IsException(mod)) {
            JSValue exc = qp_exc_fetch(ctx);
            if (!qp_exc_given_matches(ctx, exc, QP_TYPE_VALUE(QP_EXC(ctx, ImportError))) &&
                !qp_exc_given_matches(ctx, exc, QP_TYPE_VALUE(QP_EXC(ctx, AttributeError))) &&
                !qp_exc_given_matches(ctx, exc, QP_TYPE_VALUE(QP_EXC(ctx, TypeError))) &&
                !qp_exc_given_matches(ctx, exc, QP_TYPE_VALUE(QP_EXC(ctx, ValueError)))) {
                JS_Throw(ctx, exc);
                return qp_main_err_print(ctx, m);
            }
            name = qp_str_from_cstr(ctx, modname);
            if (!JS_IsException(name))
                qp_main_write_stderr_value(ctx, qp_str_from_format(ctx,
                    "%s: Error while finding module specification for %R (%T: %S)\n",
                    exe, name, exc, exc));
            JS_FreeValue(ctx, name);
            JS_FreeValue(ctx, exc);
            return 1;
        }
        JS_FreeValue(ctx, mod);
    }
    if (qp_inittab_find(modname, len))
        fprintf(stderr, "%s: No code object available for %s\n", exe, modname);
    else
        fprintf(stderr, "%s: No module named %s\n", exe, modname);
    return 1;
}

/* ---- interactive loop ------------------------------------------------- */

/* str(sys.<name>) as UTF-8 into buf ("" if missing or failing). */
static void qp_repl_prompt(JSContext *ctx, JSAtom name, DynBuf *buf)
{
    JSValue v, s;
    const char *p;

    buf->size = 0;
    v = qp_sys_get(ctx, name);
    if (JS_IsException(v)) {
        JS_FreeValue(ctx, JS_GetException(ctx));
        return;
    }
    if (QP_IS_NULL(v))
        return;
    s = qp_str(ctx, v);
    JS_FreeValue(ctx, v);
    if (JS_IsException(s)) {
        JS_FreeValue(ctx, JS_GetException(ctx));
        return;
    }
    p = qp_str_utf8(ctx, s, NULL);
    JS_FreeValue(ctx, s);
    if (!p) {
        JS_FreeValue(ctx, JS_GetException(ctx));
        return;
    }
    dbuf_putstr(buf, p);
    JS_FreeCString(ctx, p);
}

#ifdef CONFIG_PYTHON_CAPI
/* python/capi_runtime.c: PyOS_Readline once the readline module is loaded */
static BOOL qp_capi_readline_hooked(JSContext *ctx);
static int qp_capi_repl_readline(JSContext *ctx, const char *prompt, DynBuf *line);
#endif

/* PyOS_StdioReadline: prompt on stderr, one line (without '\n') appended
   to line.  1 = line read, 0 = EOF before any character, -1 = exception
   (KeyboardInterrupt of the readline hook).  With the readline module
   loaded (C API builds), PyOS_Readline and its line editing. */
static int qp_repl_readline(JSContext *ctx, JSAtom prompt_name, DynBuf *line)
{
    DynBuf prompt;
    int c, got = 0;

    dbuf_init(&prompt);
    qp_repl_prompt(ctx, prompt_name, &prompt);
#ifdef CONFIG_PYTHON_CAPI
    if (qp_capi_readline_hooked(ctx)) {
        dbuf_putc(&prompt, '\0');
        got = qp_capi_repl_readline(ctx, prompt.buf ? (const char *)prompt.buf : "", line);
        dbuf_free(&prompt);
        return got;
    }
#endif
    fwrite(prompt.buf ? (const char *)prompt.buf : "", 1, prompt.size, stderr);
    fflush(stderr);
    dbuf_free(&prompt);
    /* PyOS_Readline releases the GIL (line is a libc-allocated DynBuf) */
    QP_BEGIN_ALLOW_THREADS(ctx)
    while ((c = getc(stdin)) != EOF) {
        got = 1;
        if (c == '\n')
            break;
        dbuf_putc(line, c);
    }
    if (c == EOF)
        clearerr(stdin);
    QP_END_ALLOW_THREADS(ctx)
    return got;
}

/* Every line blank or a comment (codeop replaces such input by "pass"). */
static int qp_repl_is_blank(const char *s, size_t len)
{
    size_t i = 0;

    while (i < len) {
        while (i < len && (s[i] == ' ' || s[i] == '\t' || s[i] == '\f' || s[i] == '\r'))
            i++;
        if (i < len && s[i] != '\n' && s[i] != '#')
            return 0;
        while (i < len && s[i] != '\n')
            i++;
        i++;
    }
    return 1;
}

/* Parse check with codeop's flags: 1 parses, 0 incomplete, -1 other error. */
static int qp_repl_try_parse(JSContext *ctx, const char *src, size_t len)
{
    qp_error err;
    qp_arena *arena;
    mod_ty mod;
    int rc;

    memset(&err, 0, sizeof(err));
    arena = qp_arena_new(ctx, &err);
    if (!arena) {
        qp_error_clear(&err);
        return -1;
    }
    /* codeop._maybe_compile ignores SyntaxWarnings */
    rc = qp_parse(ctx, arena, src, len, "<stdin>", JS_UNDEFINED, QP_INPUT_SINGLE,
                  QP_PF_IGNORE_COOKIE | QP_PF_DONT_IMPLY_DEDENT | QP_PF_ALLOW_INCOMPLETE_INPUT,
                  &mod, &err);
    qp_arena_free(arena);
    if (rc == 0)
        rc = 1;
    else
        rc = err.kind == QP_ERR_INCOMPLETE ? 0 : -1;
    qp_error_clear(&err);
    return rc;
}

/* codeop._maybe_compile decision on the accumulated input: 1 complete
   (compile it, possibly to report its SyntaxError), 0 needs more lines. */
static int qp_repl_input_complete(JSContext *ctx, DynBuf *src)
{
    int rc;

    if (qp_repl_try_parse(ctx, (const char *)src->buf, src->size) > 0)
        return 1;
    dbuf_putc(src, '\n');
    dbuf_putc(src, '\0');
    src->size--;
    rc = qp_repl_try_parse(ctx, (const char *)src->buf, src->size);
    src->size--;
    src->buf[src->size] = '\0';
    return rc >= 0 ? 0 : 1;
}

/* _PyRun_InteractiveOne for one complete input: parse as "<stdin>",
   compile as "<stdin-N>" (N = *pcount, incremented, source registered for
   tracebacks) with the compile flags, run in __main__ with print_expr.
   Returns the result of the code (a coroutine with QP_PF_TOP_LEVEL_AWAIT
   if the input awaits) or JS_EXCEPTION. */
static JSValue qp_repl_exec(JSContext *ctx, const char *src, size_t len, int *pcount,
                            int flags)
{
    char filename[32];
    qp_error err;
    qp_arena *arena;
    mod_ty mod;
    JSValue main_module, d, bc, res;

    memset(&err, 0, sizeof(err));
    arena = qp_arena_new(ctx, &err);
    if (!arena) {
        qp_error_clear(&err);
        qp_raise_memory_error(ctx);
        return JS_EXCEPTION;
    }
    if (qp_parse(ctx, arena, src, len, "<stdin>", QP_NULL, QP_INPUT_SINGLE, flags, &mod,
                 &err) < 0) {
        qp_arena_free(arena);
        qp_raise_qp_error(ctx, &err, "<stdin>");
        qp_error_clear(&err);
        return JS_EXCEPTION;
    }
    snprintf(filename, sizeof(filename), "<stdin-%d>", (*pcount)++);
    if (qp_linecache_register(ctx, filename, src, len) < 0) {
        qp_arena_free(arena);
        return JS_EXCEPTION;
    }
    bc = qp_compile_ast(ctx, mod, filename, QP_NULL, QP_INPUT_SINGLE, flags, -1, arena, &err);
    qp_arena_free(arena);
    if (JS_IsException(bc)) {
        if (err.kind != QP_ERR_NONE)
            qp_raise_qp_error(ctx, &err, filename);
        qp_error_clear(&err);
        return JS_EXCEPTION;
    }
    qp_error_clear(&err);
    main_module = qp_run_main_module(ctx);
    if (JS_IsException(main_module)) {
        JS_FreeValue(ctx, bc);
        return JS_EXCEPTION;
    }
    d = qp_run_main_dict(ctx, main_module);
    if (JS_IsException(d)) {
        JS_FreeValue(ctx, bc);
        JS_FreeValue(ctx, main_module);
        return JS_EXCEPTION;
    }
    res = qp_run_code(ctx, bc, d, d);
    JS_FreeValue(ctx, d);
    JS_FreeValue(ctx, main_module);
    return res;
}

/* the interactive loop's input: 0 or -1 with exception pending */
static int qp_repl_run_one(JSContext *ctx, const char *src, size_t len, int *pcount)
{
    JSValue res = qp_repl_exec(ctx, src, len, pcount, QP_PF_IGNORE_COOKIE);

    if (JS_IsException(res))
        return -1;
    JS_FreeValue(ctx, res);
    qp_run_flush_io(ctx);
    return 0;
}

/* Default sys.ps1 / sys.ps2. */
static void qp_repl_default_prompt(JSContext *ctx, JSAtom name, const char *text)
{
    JSValue v = qp_sys_get(ctx, name);

    if (JS_IsException(v)) {
        JS_FreeValue(ctx, JS_GetException(ctx));
        return;
    }
    if (!QP_IS_NULL(v)) {
        JS_FreeValue(ctx, v);
        return;
    }
    v = qp_str_from_cstr(ctx, text);
    if (JS_IsException(v) || qp_setattr(ctx, QP_STATE(ctx)->sys_module, name, v) < 0)
        JS_FreeValue(ctx, JS_GetException(ctx));
    JS_FreeValue(ctx, v);
}

/* _PyRun_InteractiveLoop.  Returns 0 at EOF, -1 with SystemExit pending
   (without -i), which the caller turns into the exit status. */
static int qp_repl_loop(JSContext *ctx, QPMain *m)
{
    DynBuf src;
    int count = 0, rc, nomem_count = 0, more;
    JSValue exc;

    qp_repl_default_prompt(ctx, QP_ATOM(ctx, ps1), ">>> ");
    qp_repl_default_prompt(ctx, QP_ATOM(ctx, ps2), "... ");
    dbuf_init(&src);
    for (;;) {
        /* read one complete input */
        src.size = 0;
        more = 0;
        for (;;) {
            if (more)
                dbuf_putc(&src, '\n');
            rc = qp_repl_readline(ctx, more ? QP_ATOM(ctx, ps2) : QP_ATOM(ctx, ps1), &src);
            dbuf_putc(&src, '\0');
            src.size--;
            if (rc < 0)
                break;  /* KeyboardInterrupt while reading: drop the input */
            if (rc == 0) {
                if (!more) {
                    dbuf_free(&src);
                    return 0;
                }
                break;  /* EOF inside a statement: compile what we have */
            }
            if (!more && qp_repl_is_blank((const char *)src.buf, src.size)) {
                src.size = 0;
                continue;
            }
            if (qp_repl_input_complete(ctx, &src))
                break;
            more = 1;
        }
        if (rc < 0) {
            qp_main_err_print(ctx, m);
            continue;
        }
        dbuf_putc(&src, '\n');
        dbuf_putc(&src, '\0');
        src.size--;
        if (qp_repl_run_one(ctx, (const char *)src.buf, src.size, &count) == 0) {
            nomem_count = 0;
            continue;
        }
        if (qp_exc_pending_matches(ctx, QP_EXC(ctx, MemoryError))) {
            if (++nomem_count > 16) {
                dbuf_free(&src);
                return -1;
            }
        } else {
            nomem_count = 0;
        }
        if (!QP_STATE(ctx)->config.inspect &&
            qp_exc_pending_matches(ctx, QP_EXC(ctx, SystemExit))) {
            dbuf_free(&src);
            return -1;
        }
        exc = JS_GetException(ctx);
        JS_Throw(ctx, exc);
        qp_main_err_print(ctx, m);
    }
}

/* pymain_run_startup: $PYTHONSTARTUP before the interactive loop.
   Returns 1 (with *exitcode) when it raised SystemExit. */
static int qp_main_run_startup(JSContext *ctx, QPMain *m, int *exitcode)
{
    const char *startup;
    JSValue fname, res;
    char *src;
    size_t len;
    FILE *fp;
    int err;

    if (m->cfg.ignore_environment)
        return 0;
    startup = getenv("PYTHONSTARTUP");
    if (!startup || !*startup)
        return 0;
    fp = fopen(startup, "r");
    if (!fp) {
        err = errno;
        qp_sys_write_stderr(ctx, "Could not open PYTHONSTARTUP\n");
        fname = qp_str_from_cstr(ctx, startup);
        if (!JS_IsException(fname)) {
            qp_raise_os_error(ctx, err, fname);
            JS_FreeValue(ctx, fname);
        }
        qp_main_err_print(ctx, m);
        return 0;
    }
    src = qp_main_read_all(fp, &len);
    fclose(fp);
    if (!src) {
        qp_raise_memory_error(ctx);
        qp_main_err_print(ctx, m);
        return 0;
    }
    res = qp_run_simple_file(ctx, src, len, startup);
    qp_sys_free(src);
    if (JS_IsException(res)) {
        if (!m->cfg.inspect && qp_exc_pending_matches(ctx, QP_EXC(ctx, SystemExit))) {
            *exitcode = qp_main_err_print(ctx, m);
            return 1;
        }
        qp_main_err_print(ctx, m);
        return 0;
    }
    JS_FreeValue(ctx, res);
    return 0;
}

/* _pymain_run_repl: stdin as a script, or the interactive loop. */
static int qp_main_run_repl(JSContext *ctx, QPMain *m)
{
    char *src;
    size_t len;
    JSValue res;

    if (isatty(fileno(stdin)) || m->cfg.interactive) {
        if (qp_repl_loop(ctx, m) < 0)
            return qp_main_err_print(ctx, m);
        return 0;
    }
    src = qp_main_read_all(stdin, &len);
    if (!src) {
        qp_raise_memory_error(ctx);
        return qp_main_err_print(ctx, m);
    }
    res = qp_run_simple_file(ctx, src, len, "<stdin>");
    qp_sys_free(src);
    if (JS_IsException(res))
        return qp_main_err_print(ctx, m);
    JS_FreeValue(ctx, res);
    return 0;
}

/* pymain_run_stdin */
static int qp_main_run_stdin(JSContext *ctx, QPMain *m)
{
    int exitcode;

    if (qp_main_stdin_is_interactive(m)) {
        /* do exit on SystemExit */
        m->cfg.inspect = 0;
        QP_STATE(ctx)->config.inspect = 0;
        if (qp_main_run_startup(ctx, m, &exitcode))
            return exitcode;
    }
    return qp_main_run_repl(ctx, m);
}

/* pymain_repl: -i / PYTHONINSPECT after running code. */
static void qp_main_repl(JSContext *ctx, QPMain *m, int *exitcode)
{
    const char *e;

    if (!m->cfg.inspect && !m->cfg.ignore_environment && (e = getenv("PYTHONINSPECT")) && *e) {
        m->cfg.inspect = 1;
        QP_STATE(ctx)->config.inspect = 1;
    }
    if (!(m->cfg.inspect && qp_main_stdin_is_interactive(m) && qp_main_run_code(m)))
        return;
    m->cfg.inspect = 0;
    QP_STATE(ctx)->config.inspect = 0;
    *exitcode = qp_main_run_repl(ctx, m);
}

/* pymain_header */
static void qp_main_header(QPMain *m)
{
    if (m->cfg.quiet)
        return;
    if (!m->cfg.verbose && (qp_main_run_code(m) || !qp_main_stdin_is_interactive(m)))
        return;
    fprintf(stderr, "Python " QP_PY_VERSION " (main) [dquickts " CONFIG_VERSION "] on %s\n",
#if defined(__APPLE__)
            "darwin"
#elif defined(__linux__)
            "linux"
#elif defined(_WIN32)
            "win32"
#else
            "unknown"
#endif
            );
    if (m->site_import)
        fprintf(stderr, "Type \"help\", \"copyright\", \"credits\" or \"license\" for more information.\n");
}

/* ---- command line parsing (Python/getopt.c, Python/initconfig.c) ------ */

/* fprint_help: render the #X{...} markup (colors only on a terminal). */
static void qp_fprint_help(FILE *f, const char *text, const char *program)
{
    int colorize = 0;
    const char *e, *seq;
    char code;

    e = getenv("PYTHON_COLORS");
    if (e && (!strcmp(e, "0") || !strcmp(e, "1")))
        colorize = e[0] == '1';
    else if (getenv("NO_COLOR"))
        colorize = 0;
    else if (getenv("FORCE_COLOR"))
        colorize = 1;
    else
        colorize = isatty(fileno(f)) && (!(e = getenv("TERM")) || strcmp(e, "dumb") != 0);

    while (*text) {
        if (text[0] == '#' && text[1]) {
            code = text[1];
            if (text[2] == '{') {
                switch (code) {
                case 'h': seq = "\x1b[1;34m"; break;
                case 'E': seq = "\x1b[1;36m"; break;
                case 'e': seq = "\x1b[36m"; break;
                case 'L': seq = "\x1b[1;36m"; break;
                case 'b': seq = "\x1b[1;33m"; break;
                case 'B': seq = "\x1b[33m"; break;
                case 's': seq = "\x1b[1;32m"; break;
                case 'S': seq = "\x1b[32m"; break;
                default: seq = NULL; break;
                }
                if (colorize && seq)
                    fputs(seq, f);
                text += 3;
                continue;
            }
            switch (code) {
            case 'P':
                if (colorize)
                    fputs("\x1b[1;35m", f);
                if (program)
                    fputs(program, f);
                if (colorize)
                    fputs("\x1b[0m", f);
                break;
            case 'D':
                fputc(':', f);
                break;
            case 'H':
                fputs("<prefix>/lib/pythonX.X", f);
                break;
            default:
                fputc('#', f);
                fputc(code, f);
                break;
            }
            text += 2;
            continue;
        }
        if (*text == '}') {
            if (colorize)
                fputs("\x1b[0m", f);
            text++;
            continue;
        }
        fputc(*text++, f);
    }
}

static const char qp_usage_line[] =
"#h{usage:} #P [#S{option}] #S{...} "
"[#S{-c} #B{cmd} | #S{-m} #B{mod} | #S{file} | #S{-}] "
"[#S{arg}] #S{...}\n";

/* config_usage */
static void qp_config_usage(int error, const char *program)
{
    FILE *f = error ? stderr : stdout;

    qp_fprint_help(f, qp_usage_line, program);
    if (error)
        fprintf(f, "Try `python -h' for more information.\n");
    else
        qp_fprint_help(f, qp_usage_help, NULL);
}

/* fatal_error() for configuration errors; returns the exit status 1. */
static int qp_config_fatal(const char *func, const char *msg)
{
    fflush(stdout);
    if (func)
        fprintf(stderr, "Fatal Python error: %s: %s\nPython runtime state: preinitialized\n\n", func, msg);
    else
        fprintf(stderr, "Fatal Python error: %s\nPython runtime state: preinitialized\n\n", msg);
    fflush(stderr);
    return 1;
}

typedef struct QPGetOpt {
    int optind;
    const char *opt_ptr;
    const char *optarg;
} QPGetOpt;

static const struct {
    const char *name;
    int has_arg;
    int val;
} qp_longopts[] = {
    { "check-hash-based-pycs", 1, 0 },
    { "help-all", 0, 1 },
    { "help-env", 0, 2 },
    { "help-xoptions", 0, 3 },
};

#define QP_SHORT_OPTS "bBc:dEhiIJm:OPqRsStuvVW:xX:?"

/* _PyOS_GetOpt: an option character, a long option value 0..3, '_' on
   error, -1 at the end of the options. */
static int qp_getopt(QPGetOpt *g, int argc, char **argv)
{
    const char *ptr;
    char option;
    size_t i;

    if (*g->opt_ptr == '\0') {
        if (g->optind >= argc)
            return -1;
        if (argv[g->optind][0] != '-' || argv[g->optind][1] == '\0')
            return -1;
        if (strcmp(argv[g->optind], "--") == 0) {
            ++g->optind;
            return -1;
        }
        if (strcmp(argv[g->optind], "--help") == 0) {
            ++g->optind;
            return 'h';
        }
        if (strcmp(argv[g->optind], "--version") == 0) {
            ++g->optind;
            return 'V';
        }
        g->opt_ptr = &argv[g->optind++][1];
    }
    if ((option = *g->opt_ptr++) == '\0')
        return -1;
    if (option == '-') {
        if (*g->opt_ptr == '\0') {
            fprintf(stderr, "Expected long option\n");
            return -1;
        }
        for (i = 0; i < countof(qp_longopts); i++) {
            if (!strcmp(qp_longopts[i].name, g->opt_ptr))
                break;
        }
        if (i == countof(qp_longopts)) {
            fprintf(stderr, "Unknown option: %s\n", argv[g->optind - 1]);
            return '_';
        }
        g->opt_ptr = "";
        if (!qp_longopts[i].has_arg)
            return qp_longopts[i].val;
        if (g->optind >= argc) {
            fprintf(stderr, "Argument expected for the %s options\n", argv[g->optind - 1]);
            return '_';
        }
        g->optarg = argv[g->optind++];
        return qp_longopts[i].val;
    }
    if ((ptr = strchr(QP_SHORT_OPTS, option)) == NULL || option == ':') {
        fprintf(stderr, "Unknown option: -%c\n", option);
        return '_';
    }
    if (ptr[1] == ':') {
        if (*g->opt_ptr != '\0') {
            g->optarg = g->opt_ptr;
            g->opt_ptr = "";
        } else {
            if (g->optind >= argc) {
                fprintf(stderr, "Argument expected for the -%c option\n", option);
                return '_';
            }
            g->optarg = argv[g->optind++];
        }
    }
    return option;
}

/* _Py_get_env_flag: raise *flag to the value of an integer variable (a
   non-integer non-empty value counts as 1). */
static void qp_env_flag(const QPMain *m, int *flag, const char *name)
{
    const char *v;
    char *end;
    long n;

    if (m->cfg.ignore_environment)
        return;
    v = getenv(name);
    if (!v || !*v)
        return;
    n = strtol(v, &end, 10);
    if (*end != '\0' || n < 0)
        n = 1;
    if (*flag < n)
        *flag = (int)n;
}

static const char *qp_env(const QPMain *m, const char *name)
{
    const char *v;

    if (m->cfg.ignore_environment)
        return NULL;
    v = getenv(name);
    return v && *v ? v : NULL;
}

/* -X option value: "name" -> "" , "name=v" -> "v", NULL if absent (the
   last occurrence wins). */
static const char *qp_xoption(const QPMain *m, const char *name)
{
    const char *res = NULL, *x;
    size_t n = strlen(name);
    int i;

    for (i = 0; i < m->cfg.nxoptions; i++) {
        x = m->cfg.xoptions[i];
        if (!strncmp(x, name, n) && (x[n] == '\0' || x[n] == '='))
            res = x[n] ? x + n + 1 : "";
    }
    return res;
}

/* tracemalloc number of frames (_Py_str_to_int, >= 0) */
static int qp_parse_nframe(const char *s, int *out)
{
    char *end;
    long v;

    errno = 0;
    v = strtol(s, &end, 10);
    if (!*s || *end || errno == ERANGE || v < 0 || v > INT_MAX)
        return -1;
    *out = (int)v;
    return 0;
}

/* int_max_str_digits value check: 0 or >= 640. */
static int qp_parse_max_digits(const char *s, int *out)
{
    char *end;
    long v = strtol(s, &end, 10);

    if (!*s || *end || v < 0 || v > INT_MAX || (v != 0 && v < 640))
        return -1;
    *out = (int)v;
    return 0;
}

/* getpath's abspath(): `path` made absolute against the current directory
   and normalized lexically ("." and ".." components, repeated '/'),
   without resolving symbolic links.  Written to buf (PATH_MAX); NULL if
   it does not fit or the directory is unknown. */
static char *qp_main_abspath(const char *path, char *buf)
{
    char tmp[PATH_MAX];
    size_t n = 0, len;
    const char *s, *e;

    if (path[0] == '/') {
        if (strlen(path) >= sizeof(tmp))
            return NULL;
        strcpy(tmp, path);
    } else {
        if (!getcwd(tmp, sizeof(tmp)))
            return NULL;
        len = strlen(tmp);
        if (len + 1 + strlen(path) >= sizeof(tmp))
            return NULL;
        tmp[len] = '/';
        strcpy(tmp + len + 1, path);
    }
    buf[0] = '\0';
    for (s = tmp; *s; s = e) {
        while (*s == '/')
            s++;
        e = s;
        while (*e && *e != '/')
            e++;
        len = e - s;
        if (len == 0 || (len == 1 && s[0] == '.'))
            continue;
        if (len == 2 && s[0] == '.' && s[1] == '.') {
            while (n > 0 && buf[n - 1] != '/')
                n--;
            if (n > 0)
                n--;
            buf[n] = '\0';
            continue;
        }
        buf[n++] = '/';
        memcpy(buf + n, s, len);
        n += len;
        buf[n] = '\0';
    }
    if (n == 0) {
        buf[0] = '/';
        buf[1] = '\0';
    }
    return buf;
}

/* Absolute program path (sys.executable): argv[0] if it holds a '/',
   else the first match in $PATH.  Symbolic links are not resolved, so a
   virtual environment's bin/python stays sys.executable (site.venv finds
   pyvenv.cfg next to it); the stdlib is located from its real path
   (importlib_boot.c). */
static char *qp_main_executable(const char *argv0)
{
    char buf[PATH_MAX], cand[PATH_MAX];
    const char *path, *p, *q;
    char *res;
    size_t n;

    if (!argv0 || !*argv0)
        return NULL;
    if (strchr(argv0, '/')) {
        if (qp_main_abspath(argv0, buf))
            argv0 = buf;
    } else if ((path = getenv("PATH"))) {
        for (p = path; *p; p = *q ? q + 1 : q) {
            q = strchr(p, ':');
            if (!q)
                q = p + strlen(p);
            n = q - p;
            if (n == 0 || n + strlen(argv0) + 2 > sizeof(cand))
                continue;
            memcpy(cand, p, n);
            cand[n] = '/';
            strcpy(cand + n + 1, argv0);
            if (access(cand, X_OK) == 0) {
                argv0 = qp_main_abspath(cand, buf) ? buf : cand;
                break;
            }
        }
    }
    n = strlen(argv0);
    res = qp_sys_malloc(n + 1);
    if (res)
        memcpy(res, argv0, n + 1);
    return res;
}

/* _PyPathConfig_ComputeSysPath0 (CPython does it from sys.argv). */
static char *qp_main_compute_path0(const QPMain *m)
{
    char buf[PATH_MAX];
    const char *path0;
    const char *p;
    size_t n;
    char *res;

    if (m->cfg.safe_path)
        return NULL;
    if (m->cfg.run_module) {
        if (!getcwd(buf, sizeof(buf)))
            return NULL;
        path0 = buf;
        n = strlen(buf);
    } else if (m->cfg.run_filename) {
        path0 = m->cfg.run_filename;
        if (realpath(path0, buf))
            path0 = buf;
        p = strrchr(path0, '/');
        if (p) {
            n = p + 1 - path0;
            if (n > 1)
                n--;    /* drop trailing separator */
        } else {
            n = 0;
        }
    } else {
        path0 = "";
        n = 0;
    }
    res = qp_sys_malloc(n + 1);
    if (res) {
        memcpy(res, path0, n);
        res[n] = '\0';
    }
    return res;
}

/* config_parse_cmdline + the environment and -X handling of
   config_read.  Returns -1 to continue, or the exit status. */
static int qp_main_parse(QPMain *m, int argc, char **argv)
{
    QPConfig *cfg = &m->cfg;
    QPGetOpt g = { 1, "", NULL };
    int c, print_version = 0, opt_index, nw = 0, i, bytes_warning = 0;
    const char *e, *x;
    char *p, **warn = NULL;

    m->program = argc >= 1 ? argv[0] : "python";
    cfg->optimize = 0;
    cfg->int_max_str_digits = -1;
    cfg->lazy_imports = -1;
    cfg->utf8_mode = 1;     /* PEP 686: UTF-8 mode is the default in 3.15 */
    cfg->orig_argc = argc;
    cfg->orig_argv = argv;
    cfg->xoptions = qp_sys_calloc(argc + 1, sizeof(char *));
    warn = qp_sys_calloc(argc + 4, sizeof(char *));
    cfg->warnoptions = qp_sys_calloc(argc + 4, sizeof(char *));
    if (!cfg->xoptions || !warn || !cfg->warnoptions) {
        qp_sys_free(warn);
        return qp_config_fatal("config_parse_cmdline", "memory allocation failed");
    }
    m->site_import = 1;

    for (;;) {
        g.optarg = NULL;
        c = qp_getopt(&g, argc, argv);
        if (c == -1)
            break;
        if (c == 'c') {
            /* -c is the last option; the text gets a trailing newline */
            size_t len = strlen(g.optarg);
            m->command = qp_sys_malloc(len + 2);
            if (!m->command) {
                qp_sys_free(warn);
                return qp_config_fatal("config_parse_cmdline", "memory allocation failed");
            }
            memcpy(m->command, g.optarg, len);
            m->command[len] = '\n';
            m->command[len + 1] = '\0';
            cfg->run_command = m->command;
            break;
        }
        if (c == 'm') {
            cfg->run_module = g.optarg;
            break;
        }
        switch (c) {
        case 0:
            if (strcmp(g.optarg, "always") && strcmp(g.optarg, "never") &&
                strcmp(g.optarg, "default")) {
                fprintf(stderr, "--check-hash-based-pycs must be one of "
                        "'default', 'always', or 'never'\n");
                qp_config_usage(1, m->program);
                qp_sys_free(warn);
                return 2;
            }
            cfg->check_hash_based_pycs = g.optarg;
            break;
        case 1:
            qp_config_usage(0, m->program);
            putchar('\n');
            qp_fprint_help(stdout, qp_usage_envvars, NULL);
            putchar('\n');
            qp_fprint_help(stdout, qp_usage_xoptions, NULL);
            qp_sys_free(warn);
            return 0;
        case 2:
            qp_fprint_help(stdout, qp_usage_envvars, NULL);
            qp_sys_free(warn);
            return 0;
        case 3:
            qp_fprint_help(stdout, qp_usage_xoptions, NULL);
            qp_sys_free(warn);
            return 0;
        case 'b':
            bytes_warning++;
            break;
        case 'd':
            break;  /* parser debugging: debug builds only */
        case 'i':
            cfg->inspect++;
            cfg->interactive++;
            break;
        case 'E':
            cfg->ignore_environment = 1;
            break;
        case 'I':
            cfg->isolated = 1;
            cfg->ignore_environment = 1;
            cfg->safe_path = 1;
            cfg->no_user_site = 1;
            break;
        case 'X':
            cfg->xoptions[cfg->nxoptions++] = (char *)g.optarg;
            break;
        case 'O':
            cfg->optimize++;
            break;
        case 'P':
            cfg->safe_path = 1;
            break;
        case 'B':
            cfg->dont_write_bytecode = 1;
            break;
        case 's':
            cfg->no_user_site = 1;
            break;
        case 'S':
            cfg->no_site = 1;
            m->site_import = 0;
            break;
        case 't':
            break;  /* ignored for backwards compatibility */
        case 'u':
            cfg->unbuffered = 1;
            break;
        case 'v':
            cfg->verbose++;
            break;
        case 'x':
            m->skip_first_line = 1;
            break;
        case 'h':
        case '?':
            qp_config_usage(0, m->program);
            qp_sys_free(warn);
            return 0;
        case 'V':
            print_version++;
            break;
        case 'W':
            warn[nw++] = (char *)g.optarg;
            break;
        case 'q':
            cfg->quiet++;
            break;
        case 'R':
            break;  /* hash randomization is always on unless PYTHONHASHSEED */
        default:
            qp_config_usage(1, m->program);
            qp_sys_free(warn);
            return 2;
        }
    }
    if (print_version) {
        if (print_version >= 2)
            printf("Python " QP_PY_VERSION " (main) [dquickts " CONFIG_VERSION "]\n");
        else
            printf("Python " QP_PY_VERSION "\n");
        qp_sys_free(warn);
        return 0;
    }
    if (!cfg->run_command && !cfg->run_module && g.optind < argc && strcmp(argv[g.optind], "-") != 0) {
        /* config_run_filename_abspath: on failure keep the relative path */
        char cwd[PATH_MAX];
        const char *fn = argv[g.optind];
        cfg->run_filename = fn;
        if (fn[0] != '/' && getcwd(cwd, sizeof(cwd))) {
            size_t cl = strlen(cwd), fl = strlen(fn);
            m->run_filename = qp_sys_malloc(cl + fl + 2);
            if (!m->run_filename) {
                qp_sys_free(warn);
                return qp_config_fatal("config_run_filename_abspath", "memory allocation failed");
            }
            memcpy(m->run_filename, cwd, cl);
            m->run_filename[cl] = '/';
            memcpy(m->run_filename + cl + 1, fn, fl + 1);
            cfg->run_filename = m->run_filename;
        }
    }
    if (cfg->run_command || cfg->run_module)
        g.optind--;
    opt_index = g.optind;

    /* sys.argv */
    m->sys_argv = qp_sys_calloc(argc - opt_index + 2, sizeof(char *));
    if (!m->sys_argv) {
        qp_sys_free(warn);
        return qp_config_fatal("config_init_argv", "memory allocation failed");
    }
    if (opt_index >= argc) {
        m->sys_argv[0] = (char *)"";
        cfg->argc = 1;
    } else {
        for (i = opt_index; i < argc; i++)
            m->sys_argv[i - opt_index] = argv[i];
        cfg->argc = argc - opt_index;
    }
    if (cfg->run_command)
        m->sys_argv[0] = (char *)"-c";
    else if (cfg->run_module)
        m->sys_argv[0] = (char *)"-m";
    cfg->argv = m->sys_argv;

    /* environment variables (ignored with -E / -I) */
    qp_env_flag(m, &cfg->optimize, "PYTHONOPTIMIZE");
    qp_env_flag(m, &cfg->inspect, "PYTHONINSPECT");
    qp_env_flag(m, &cfg->verbose, "PYTHONVERBOSE");
    qp_env_flag(m, &cfg->dont_write_bytecode, "PYTHONDONTWRITEBYTECODE");
    qp_env_flag(m, &cfg->no_user_site, "PYTHONNOUSERSITE");
    qp_env_flag(m, &cfg->unbuffered, "PYTHONUNBUFFERED");
    qp_env_flag(m, &cfg->dev_mode, "PYTHONDEVMODE");
    qp_env_flag(m, &cfg->warn_default_encoding, "PYTHONWARNDEFAULTENCODING");
    if (qp_env(m, "PYTHONSAFEPATH"))
        cfg->safe_path = 1;
    cfg->pythonpath = qp_env(m, "PYTHONPATH");
    cfg->stdio_encoding = qp_env(m, "PYTHONIOENCODING");
    if ((e = qp_env(m, "PYTHONHASHSEED"))) {
        if (strcmp(e, "random") != 0) {
            char *end;
            unsigned long seed;
            errno = 0;
            seed = strtoul(e, &end, 10);
            if (*end != '\0' || seed > 4294967295UL || (errno == ERANGE && seed == ULONG_MAX) ||
                e[0] == '-') {
                qp_sys_free(warn);
                return qp_config_fatal("config_init_hash_seed",
                                       "PYTHONHASHSEED must be \"random\" or an integer in range "
                                       "[0; 4294967295]");
            }
        }
        cfg->hash_seed = e;
    }
    if ((e = qp_env(m, "PYTHONUTF8"))) {
        if (!strcmp(e, "1"))
            cfg->utf8_mode = 1;
        else if (!strcmp(e, "0"))
            cfg->utf8_mode = 0;
        else {
            qp_sys_free(warn);
            return qp_config_fatal("preconfig_init_utf8_mode",
                                   "invalid PYTHONUTF8 environment variable value");
        }
    }
    if ((e = qp_env(m, "PYTHONINTMAXSTRDIGITS")) &&
        qp_parse_max_digits(e, &cfg->int_max_str_digits) < 0) {
        qp_sys_free(warn);
        return qp_config_fatal("config_init_int_max_str_digits",
                               "PYTHONINTMAXSTRDIGITS: invalid limit; must be >= 640 "
                               "or 0 for unlimited.");
    }
    /* config_init_import_time */
    if ((e = qp_env(m, "PYTHONPROFILEIMPORTTIME"))) {
        char *end;
        long v = strtol(e, &end, 10);
        if (!*e || *end)
            v = 1;
        if (v < 0 || v > 2) {
            qp_sys_free(warn);
            return qp_config_fatal("config_init_import_time",
                                   "PYTHONPROFILEIMPORTTIME: numeric values other than 1 and 2 "
                                   "are reserved for future use.");
        }
        cfg->import_time = (int)v;
    }
    if ((x = qp_xoption(m, "importtime"))) {
        char *end;
        long v = strtol(x, &end, 10);
        if (!*x || *end)
            v = 1;
        if (v < 0 || v > 2) {
            qp_sys_free(warn);
            return qp_config_fatal("config_init_import_time",
                                   "-X importtime: values other than 1 and 2 "
                                   "are reserved for future use.");
        }
        cfg->import_time = (int)v;
    }
    /* config_init_tracemalloc: the number is checked by
       _PyTraceMalloc_Start() (qp_main_start_tracemalloc) */
    if ((e = qp_env(m, "PYTHONTRACEMALLOC"))) {
        if (qp_parse_nframe(e, &cfg->tracemalloc) < 0) {
            qp_sys_free(warn);
            return qp_config_fatal("config_init_tracemalloc",
                                   "PYTHONTRACEMALLOC: invalid number of frames");
        }
    }
    if ((x = qp_xoption(m, "tracemalloc"))) {
        if (!*x) {
            cfg->tracemalloc = 1;
        } else if (qp_parse_nframe(x, &cfg->tracemalloc) < 0) {
            qp_sys_free(warn);
            return qp_config_fatal("config_init_tracemalloc",
                                   "-X tracemalloc=NFRAME: invalid number of frames");
        }
    }
    /* config_init_import: the frozen modules are the base snapshot */
    m->use_frozen_modules = 1;
    if ((e = qp_env(m, "PYTHON_FROZEN_MODULES"))) {
        if (!strcmp(e, "on"))
            m->use_frozen_modules = 1;
        else if (!strcmp(e, "off"))
            m->use_frozen_modules = 0;
        else {
            qp_sys_free(warn);
            return qp_config_fatal(NULL, "bad value for PYTHON_FROZEN_MODULES "
                                   "(expected \"on\" or \"off\")");
        }
    }
    if ((x = qp_xoption(m, "frozen_modules"))) {
        if (!strcmp(x, "on") || !*x)
            m->use_frozen_modules = 1;
        else if (!strcmp(x, "off"))
            m->use_frozen_modules = 0;
        else {
            qp_sys_free(warn);
            return qp_config_fatal(NULL, "bad value for option -X frozen_modules "
                                   "(expected \"on\" or \"off\")");
        }
    }
    /* config_init_pycache_prefix: "-X pycache_prefix=" (empty) ignores
       PYTHONPYCACHEPREFIX */
    if ((x = qp_xoption(m, "pycache_prefix")))
        cfg->pycache_prefix = *x ? x : NULL;
    else if ((e = qp_env(m, "PYTHONPYCACHEPREFIX")) && *e)
        cfg->pycache_prefix = e;
    if ((e = qp_env(m, "PYTHON_LAZY_IMPORTS"))) {
        if (!strcmp(e, "all"))
            cfg->lazy_imports = 1;
        else if (!strcmp(e, "normal"))
            cfg->lazy_imports = -1;
        else {
            qp_sys_free(warn);
            return qp_config_fatal("config_init_lazy_imports",
                                   "PYTHON_LAZY_IMPORTS: invalid value; expected 'all' or 'normal'");
        }
    }

    /* -X options (override the environment) */
    if ((x = qp_xoption(m, "utf8"))) {
        if (!*x || !strcmp(x, "1"))
            cfg->utf8_mode = 1;
        else if (!strcmp(x, "0"))
            cfg->utf8_mode = 0;
        else {
            qp_sys_free(warn);
            return qp_config_fatal("preconfig_init_utf8_mode", "invalid -X utf8 option value");
        }
    }
    if (qp_xoption(m, "dev"))
        cfg->dev_mode = 1;
    if (qp_xoption(m, "warn_default_encoding"))
        cfg->warn_default_encoding = 1;
    if ((x = qp_xoption(m, "int_max_str_digits")) &&
        qp_parse_max_digits(x, &cfg->int_max_str_digits) < 0) {
        qp_sys_free(warn);
        return qp_config_fatal("config_init_int_max_str_digits",
                               "-X int_max_str_digits: invalid limit; must be >= 640 "
                               "or 0 for unlimited.");
    }
    if ((x = qp_xoption(m, "lazy_imports"))) {
        if (!strcmp(x, "all"))
            cfg->lazy_imports = 1;
        else if (!strcmp(x, "normal"))
            cfg->lazy_imports = -1;
        else {
            qp_sys_free(warn);
            return qp_config_fatal("config_init_lazy_imports",
                                   "-X lazy_imports: invalid value; expected 'all' or 'normal'");
        }
    }

    /* sys.warnoptions, lowest precedence first: dev mode, PYTHONWARNINGS,
       -W options, then the -b/-bb BytesWarning filter */
    if (cfg->dev_mode)
        cfg->warnoptions[cfg->nwarnoptions++] = (char *)"default";
    if ((e = qp_env(m, "PYTHONWARNINGS"))) {
        size_t n = strlen(e);
        int count = 1;
        for (i = 0; e[i]; i++)
            count += e[i] == ',';
        m->envwarn = qp_sys_malloc(n + 1);
        if (m->envwarn) {
            char **nw2 = qp_sys_realloc(cfg->warnoptions, (argc + 4 + count) * sizeof(char *));
            if (nw2) {
                cfg->warnoptions = nw2;
                memcpy(m->envwarn, e, n + 1);
                for (p = m->envwarn;;) {
                    char *comma = strchr(p, ',');
                    if (comma)
                        *comma = '\0';
                    if (*p)
                        cfg->warnoptions[cfg->nwarnoptions++] = p;
                    if (!comma)
                        break;
                    p = comma + 1;
                }
            }
        }
    }
    for (i = 0; i < nw; i++)
        cfg->warnoptions[cfg->nwarnoptions++] = warn[i];
    qp_sys_free(warn);
    cfg->bytes_warning = bytes_warning;
    if (bytes_warning)
        cfg->warnoptions[cfg->nwarnoptions++] =
            (char *)(bytes_warning > 1 ? "error::BytesWarning" : "default::BytesWarning");

    m->executable = qp_main_executable(m->program);
    cfg->executable = m->executable ? m->executable : m->program;
    m->path0 = qp_main_compute_path0(m);
    cfg->sys_path_0 = m->path0;
    return -1;
}

/* Native stack for deep Python recursion: most of the thread's stack
   limit (CPython relies on the C stack the same way). */
static void qp_main_set_stack(JSRuntime *rt)
{
    struct rlimit rl;
    size_t size = 8 * 1024 * 1024;

    if (getrlimit(RLIMIT_STACK, &rl) == 0) {
        if (rl.rlim_cur == RLIM_INFINITY)
            size = 64 * 1024 * 1024;
        else if (rl.rlim_cur > 2 * 1024 * 1024)
            size = rl.rlim_cur - 512 * 1024;
        else
            size = rl.rlim_cur * 3 / 4;
    }
    if (size > (size_t)256 * 1024 * 1024)
        size = (size_t)256 * 1024 * 1024;
    JS_UpdateStackTop(rt);
    JS_SetMaxStackSize(rt, size);
}

/* sys.path.insert(0, path0) (pymain_sys_path_add_path0). */
static int qp_main_add_path0(JSContext *ctx, const char *path0)
{
    JSValue path, s;
    int rc;

    path = qp_sys_get(ctx, QP_ATOM(ctx, path));
    if (JS_IsException(path))
        return -1;
    if (QP_IS_NULL(path)) {
        qp_raise_runtime_error(ctx, "unable to get sys.path");
        return -1;
    }
    s = qp_str_from_utf8(ctx, path0, strlen(path0));
    if (JS_IsException(s)) {
        JS_FreeValue(ctx, path);
        return -1;
    }
    if (qp_list_check(ctx, path)) {
        rc = qp_list_insert(ctx, path, 0, s);
    } else {
        JSValue args[2] = { JS_NewInt32(ctx, 0), s };
        JSAtom a = JS_NewAtom(ctx, "insert");
        JSValue r = qp_call_method(ctx, path, a, (JSValueConst *)args, 2, QP_NULL);
        JS_FreeAtom(ctx, a);
        rc = JS_IsException(r) ? -1 : 0;
        JS_FreeValue(ctx, r);
    }
    JS_FreeValue(ctx, s);
    JS_FreeValue(ctx, path);
    return rc;
}

/* wait_for_thread_shutdown: threading._shutdown() when threading was
   imported (errors reported as unraisable). */
static void qp_main_thread_shutdown(JSContext *ctx)
{
    JSValue name, threading, r, exc;
    JSAtom a;
    int rc;

    name = qp_str_from_cstr(ctx, "threading");
    if (JS_IsException(name)) {
        JS_FreeValue(ctx, JS_GetException(ctx));
        return;
    }
    rc = qp_import_get_module(ctx, name, &threading);
    JS_FreeValue(ctx, name);
    if (rc < 0) {
        exc = qp_exc_fetch(ctx);
        qp_exc_write_unraisable(ctx, exc, QP_NULL);
        JS_FreeValue(ctx, exc);
        return;
    }
    if (rc == 0)
        return;
    a = JS_NewAtom(ctx, "_shutdown");
    r = qp_call_method(ctx, threading, a, NULL, 0, QP_NULL);
    JS_FreeAtom(ctx, a);
    if (JS_IsException(r)) {
        exc = qp_exc_fetch(ctx);
        qp_exc_write_unraisable(ctx, exc, QP_NULL);
        JS_FreeValue(ctx, exc);
    }
    JS_FreeValue(ctx, r);
    JS_FreeValue(ctx, threading);
}

static void qp_main_free(QPMain *m)
{
    qp_sys_free(m->sys_argv);
    qp_sys_free(m->executable);
    qp_sys_free(m->path0);
    qp_sys_free(m->command);
    qp_sys_free(m->envwarn);
    qp_sys_free(m->run_filename);
    qp_sys_free(m->cfg.xoptions);
    qp_sys_free(m->cfg.warnoptions);
}

/* ---- finalization (Py_FinalizeEx) ------------------------------------- */

static JSValue qp_weakref_new(JSContext *ctx, JSValueConst obj, JSValueConst callback);
static JSValue qp_weakref_get(JSContext *ctx, JSValueConst ref);
static void qp_weakref_run_pending(JSContext *ctx);
static void qp_gc_collect_at_exit(JSContext *ctx);   /* mod_misc.c */

/* _PyGC_CollectNoFail: free the unreachable cycles and run the __del__
   methods, finalizers and weakref callbacks of the garbage, again while
   they leave garbage with finalizers */
static void qp_finalize_collect(JSContext *ctx)
{
    QPState *s = QP_STATE(ctx);
    int i;

    for (i = 0; i < 16; i++) {
        BOOL pending;
        JS_RunGC(ctx->rt);
        pending = JS_VALUE_GET_TAG(s->pending_del) == JS_TAG_OBJECT &&
            qp_list_size(s->pending_del) > 0;
        qp_run_pending_finalizers(ctx);
        qp_weakref_run_pending(ctx);
        if (!pending)
            break;
    }
}

/* sys.<name> = value (a new reference, freed) */
static void qp_finalize_sys_set(JSContext *ctx, const char *name, JSValue value)
{
    QPState *s = QP_STATE(ctx);
    JSAtom atom = JS_NewAtom(ctx, name);

    if (atom == JS_ATOM_NULL ||
        qp_dict_set_item_atom(ctx, s->sys_dict, atom, value) < 0)
        qp_tw_clear_exc(ctx);
    JS_FreeAtom(ctx, atom);
    JS_FreeValue(ctx, value);
}

/* finalize_modules_delete_special and _PyImport_FiniExternal: builtins._,
   the import machinery and the interactive state of sys are None, the
   standard streams the original ones */
static void qp_finalize_delete_special(JSContext *ctx)
{
    static const char * const sys_deletes[] = {
        "path", "argv", "ps1", "ps2", "last_exc", "last_type", "last_value",
        "last_traceback", "__interactivehook__", "meta_path",
        "path_importer_cache", "path_hooks",
    };
    static const char * const sys_files[] = {
        "stdin", "__stdin__", "stdout", "__stdout__", "stderr", "__stderr__",
    };
    QPState *s = QP_STATE(ctx);
    JSAtom atom;
    size_t i;

    atom = JS_NewAtom(ctx, "_");
    if (atom == JS_ATOM_NULL ||
        qp_dict_set_item_atom(ctx, s->builtins_dict, atom, qp_none()) < 0)
        qp_tw_clear_exc(ctx);
    JS_FreeAtom(ctx, atom);
    for (i = 0; i < countof(sys_deletes); i++)
        qp_finalize_sys_set(ctx, sys_deletes[i], qp_none());
    for (i = 0; i < countof(sys_files); i += 2) {
        JSValue v;
        int r;
        atom = JS_NewAtom(ctx, sys_files[i + 1]);
        if (atom == JS_ATOM_NULL) {
            qp_tw_clear_exc(ctx);
            continue;
        }
        r = qp_dict_get_item_atom(ctx, s->sys_dict, atom, &v);
        JS_FreeAtom(ctx, atom);
        if (r < 0)
            qp_tw_clear_exc(ctx);
        else if (r > 0)
            qp_finalize_sys_set(ctx, sys_files[i], v);
    }
}

/* _PyModule_ClearDict: the names starting with a single underscore, then
   every name but __builtins__, are set to None. The values are not held
   meanwhile: as in CPython, a value whose last reference goes is
   finalized at once (its __del__ methods and weakref callbacks run before
   the next name is cleared), while the modules cleared later, i.e.
   imported earlier (those of sys.stderr), still work. */
static void qp_finalize_clear_dict(JSContext *ctx, JSValueConst d)
{
    JSValue keys, value;
    JSValue *names;
    Py_ssize_t i, n;
    int pass, r;

    for (pass = 0; pass < 2; pass++) {
        /* the names of the pass (a finalizer may add some) */
        keys = qp_dict_keys_list(ctx, d);
        if (JS_IsException(keys)) {
            qp_tw_clear_exc(ctx);
            return;
        }
        names = qp_list_items(keys, &n);
        for (i = 0; i < n; i++) {
            JSValueConst key = names[i];
            const char *name;
            BOOL clear;
            if (!JS_IsString(key))
                continue;
            r = qp_dict_get_item(ctx, d, key, &value);
            if (r < 0)
                qp_tw_clear_exc(ctx);
            if (r <= 0)
                continue;
            clear = !JS_IsUndefined(value);
            JS_FreeValue(ctx, value);
            if (!clear)
                continue;
            name = JS_ToCString(ctx, key);
            if (!name) {
                qp_tw_clear_exc(ctx);
                continue;
            }
            if (pass == 0)
                clear = name[0] == '_' && name[1] != '_';
            else
                clear = strcmp(name, "__builtins__") != 0;
            JS_FreeCString(ctx, name);
            if (!clear)
                continue;
            if (qp_dict_set_item(ctx, d, key, qp_none()) < 0)
                qp_tw_clear_exc(ctx);
            qp_run_pending_finalizers(ctx);
            qp_weakref_run_pending(ctx);
        }
        JS_FreeValue(ctx, keys);
    }
}

/* finalize_modules: the modules are removed from sys.modules and the
   garbage is collected, so that the __del__ methods and finalizers of the
   cyclic garbage run while the modules they use still work (asyncio's
   "Task was destroyed but it is pending!"); then the globals of the
   modules still alive, but sys and builtins, are cleared in the reverse
   order of the imports and the garbage collected again. */
static void qp_finalize_modules(JSContext *ctx)
{
    QPState *s = QP_STATE(ctx);
    JSValue items, *refs = NULL;
    JSValue *pairs;
    Py_ssize_t i, n, nrefs = 0;

    s->finalizing = 1;
    qp_finalize_delete_special(ctx);
    /* finalize_remove_modules and finalize_clear_modules_dict */
    items = qp_dict_check(ctx, s->modules) ?
        qp_dict_items_list(ctx, s->modules) : JS_EXCEPTION;
    if (JS_IsException(items)) {
        qp_tw_clear_exc(ctx);
    } else {
        pairs = qp_list_items(items, &n);
        refs = js_malloc(ctx, sizeof(refs[0]) * max_int(n, 1));
        if (!refs)
            qp_tw_clear_exc(ctx);
        for (i = 0; i < n && refs; i++) {
            JSValueConst mod = QP_TUPLE_ITEMS(pairs[i])[1];
            JSValue ref;
            if (!qp_module_check(ctx, mod))
                continue;
            ref = qp_weakref_new(ctx, mod, QP_NULL);
            if (JS_IsException(ref))
                qp_tw_clear_exc(ctx);
            else
                refs[nrefs++] = ref;
        }
        for (i = 0; i < n; i++) {
            if (qp_dict_set_item(ctx, s->modules, QP_TUPLE_ITEMS(pairs[i])[0],
                                 qp_none()) < 0)
                qp_tw_clear_exc(ctx);
        }
        JS_FreeValue(ctx, items);
        qp_dictcore_clear(ctx, &QP_DICT(s->modules)->core);
    }
    qp_finalize_collect(ctx);
    /* finalize_modules_clear_weaklist */
    for (i = nrefs - 1; i >= 0; i--) {
        JSValue mod = qp_weakref_get(ctx, refs[i]);
        if (JS_IsException(mod)) {
            qp_tw_clear_exc(ctx);
        } else if (qp_module_check(ctx, mod)) {
            JSValueConst d = qp_module_get_dict(mod);
            if (qp_dict_check(ctx, d) &&
                JS_VALUE_GET_PTR(d) != JS_VALUE_GET_PTR(s->sys_dict) &&
                JS_VALUE_GET_PTR(d) != JS_VALUE_GET_PTR(s->builtins_dict))
                qp_finalize_clear_dict(ctx, d);
        }
        JS_FreeValue(ctx, mod);
        JS_FreeValue(ctx, refs[i]);
    }
    js_free(ctx, refs);
    qp_finalize_collect(ctx);
}

/* Py_FinalizeEx up to the release of the state: threading._shutdown(),
   atexit, the other threads stopped (they hang when they try to take the
   GIL), the standard streams flushed, the modules finalized and what
   their finalizers printed flushed.  Runs once per context.  *pnthreads
   gets the number of other threads still existing.  Returns -1 when a
   flush of the standard streams failed. */
static int qp_finalize(JSContext *ctx, int *pnthreads)
{
    QPState *s = QP_STATE(ctx);
    int ret = 0;

    *pnthreads = 0;
    if (s->finalized)
        return 0;
    s->finalized = 1;
    qp_main_thread_shutdown(ctx);
    qp_atexit_run(ctx);
    *pnthreads = qp_threads_finalize(ctx);
    if (qp_sys_flush_std_files(ctx) < 0)
        ret = -1;
    /* a collection before the modules (Py_FinalizeEx's _PyGC_CollectNoFail) */
    qp_gc_collect_at_exit(ctx);
    qp_finalize_modules(ctx);
    if (qp_sys_flush_std_files(ctx) < 0)
        ret = -1;
    fflush(stdout);
    fflush(stderr);
    /* nothing is left pending for the teardown */
    if (JS_HasException(ctx))
        JS_FreeValue(ctx, JS_GetException(ctx));
    return ret;
}

/* JS_FreeRuntime, before the C API teardown and the final collection:
   qp_finalize for every context with a Python state that was not
   finalized (an embedder that used Python through JS_InitPython /
   JS_EvalPython / JS_PythonImport, contexts already released with
   JS_FreeContext included), so that the modules are finalized, their C
   state released and the garbage collected while the interpreter still
   works, as at the exit of qpy. */
static void qp_runtime_finalize(JSRuntime *rt)
{
    struct list_head *el;
    int nthreads;

 again:
    list_for_each(el, &rt->context_list) {
        JSContext *ctx = list_entry(el, JSContext, link);
        if (ctx->py && !ctx->py->finalized) {
            /* the finalizers may release the last references of ctx */
            JS_DupContext(ctx);
            qp_finalize(ctx, &nthreads);
            JS_FreeContext(ctx);
            goto again;
        }
    }
}

/* ---- the base snapshot (plan section 1) ------------------------------- */

/* qpy embeds the interpreter state after the part of the startup that
   does not depend on the process: qp_importlib_boot_core (importlib,
   zipimport, encodings, _signal), the _io classes (_pyio and the modules
   it imports: os, abc, codecs, stat...) and the UTF-8 codec, i.e. every
   module of a `python -S` start but __main__.  The build writes it with
   `qpy --write-base-snapshot FILE DIR` (qp_write_base_snapshot, DIR being
   the directory of the qpy that embeds it) and qpy.c hands it to
   qp_set_base_snapshot().  qp_main then restores it instead of building
   the state and compiling these modules, and applies what depends on the
   process like a normal start does (qp_main_attach_base): the
   configuration and the sys attributes it gives, the getpath paths, the
   hash key (the hash tables of the snapshot are rehashed with it), the
   signals inherited ignored, posix.environ, the time zone of the time
   module, the standard streams, the -W options and site.

   The snapshot is only used when the configuration gives the same state
   (qp_main_base_eligible: no -O, -v, -X importtime, dev mode, -X options
   other than utf8 and frozen_modules...), when every source file of its
   modules still has the size and modification time it had at build time
   (a stale base is never used), when the stdlib of the process is the one
   of the base, and unless -X frozen_modules=off / PYTHON_FROZEN_MODULES=off
   (CPython's switch for its frozen stdlib modules).  Otherwise qp_main
   starts normally.

   Blob layout (host byte order, read by the build that wrote it):
     "QPYBASE1"
     u32 number of source files, then for each: u32 length and absolute
         path (UTF-8, surrogateescape), i64 mtime seconds, i64 mtime
         nanoseconds, i64 size
     u32 length and sys._stdlib_dir of the base
     u64 length and the JS_WriteSnapshot() data (self-contained) */

#define QP_BASE_MAGIC "QPYBASE1"
#define QP_BASE_MAGIC_LEN 8
#ifdef NSIG
#define QP_MAIN_NSIG NSIG
#else
#define QP_MAIN_NSIG 65
#endif
#ifdef __APPLE__
#define QP_BASE_ST_MTIM(st) ((st).st_mtimespec)
#else
#define QP_BASE_ST_MTIM(st) ((st).st_mtim)
#endif

/* python/mod_codecs.c */
static JSValue qp_codec_lookup(JSContext *ctx, const char *encoding);

static const uint8_t *qp_base_data;
static size_t qp_base_size;

void qp_set_base_snapshot(const uint8_t *data, size_t size)
{
    qp_base_data = data;
    qp_base_size = size;
}

typedef struct QPBaseBlob {
    const uint8_t *files, *files_end;   /* the file records */
    uint32_t nfiles;
    const char *stdlib;
    uint32_t stdlib_len;
    const uint8_t *snap;
    size_t snap_len;
} QPBaseBlob;

typedef struct QPBaseReader {
    const uint8_t *p, *end;
} QPBaseReader;

static int qp_base_get(QPBaseReader *r, void *dst, size_t n)
{
    if ((size_t)(r->end - r->p) < n)
        return -1;
    memcpy(dst, r->p, n);
    r->p += n;
    return 0;
}

static int qp_base_skip(QPBaseReader *r, size_t n)
{
    if ((size_t)(r->end - r->p) < n)
        return -1;
    r->p += n;
    return 0;
}

/* a file record: path (not NUL-terminated), mtime and size */
static int qp_base_get_file(QPBaseReader *r, const char **path, uint32_t *len,
                            int64_t *sec, int64_t *nsec, int64_t *size)
{
    if (qp_base_get(r, len, 4) < 0)
        return -1;
    *path = (const char *)r->p;
    if (qp_base_skip(r, *len) < 0 || qp_base_get(r, sec, 8) < 0 ||
        qp_base_get(r, nsec, 8) < 0 || qp_base_get(r, size, 8) < 0)
        return -1;
    return 0;
}

static int qp_base_parse(QPBaseBlob *b)
{
    QPBaseReader r = { qp_base_data, qp_base_data + qp_base_size };
    const char *path;
    uint32_t i, len;
    int64_t sec, nsec, size;
    uint64_t snap_len;

    if (!qp_base_data || qp_base_size < QP_BASE_MAGIC_LEN ||
        memcmp(qp_base_data, QP_BASE_MAGIC, QP_BASE_MAGIC_LEN))
        return -1;
    r.p += QP_BASE_MAGIC_LEN;
    if (qp_base_get(&r, &b->nfiles, 4) < 0)
        return -1;
    b->files = r.p;
    for (i = 0; i < b->nfiles; i++) {
        if (qp_base_get_file(&r, &path, &len, &sec, &nsec, &size) < 0)
            return -1;
    }
    b->files_end = r.p;
    if (qp_base_get(&r, &b->stdlib_len, 4) < 0)
        return -1;
    b->stdlib = (const char *)r.p;
    if (qp_base_skip(&r, b->stdlib_len) < 0 || qp_base_get(&r, &snap_len, 8) < 0 ||
        snap_len != (uint64_t)(r.end - r.p))
        return -1;
    b->snap = r.p;
    b->snap_len = (size_t)snap_len;
    return 0;
}

/* every source file of the base is unchanged */
static BOOL qp_base_files_current(const QPBaseBlob *b)
{
    QPBaseReader r = { b->files, b->files_end };
    char buf[PATH_MAX];
    const char *path;
    uint32_t i, len;
    int64_t sec, nsec, size;
    struct stat st;

    for (i = 0; i < b->nfiles; i++) {
        if (qp_base_get_file(&r, &path, &len, &sec, &nsec, &size) < 0 || len >= sizeof(buf))
            return FALSE;
        memcpy(buf, path, len);
        buf[len] = '\0';
        if (stat(buf, &st) < 0 || (int64_t)st.st_size != size ||
            (int64_t)QP_BASE_ST_MTIM(st).tv_sec != sec ||
            (int64_t)QP_BASE_ST_MTIM(st).tv_nsec != nsec)
            return FALSE;
    }
    return TRUE;
}

/* x is the -X option `name` or `name=value` */
static BOOL qp_xoption_is(const char *x, const char *name)
{
    size_t n = strlen(name);
    return !strncmp(x, name, n) && (x[n] == '\0' || x[n] == '=');
}

/* the configuration gives the state of the base (see qp_base_init) */
static BOOL qp_main_base_eligible(const QPMain *m)
{
    const QPConfig *c = &m->cfg;
    int i;

    if (!qp_base_data || !m->use_frozen_modules)
        return FALSE;
    if (c->optimize || c->verbose || c->import_time || c->dev_mode ||
        c->warn_default_encoding || c->int_max_str_digits != -1 || c->lazy_imports != -1 ||
        c->recursion_limit || !c->utf8_mode || c->home || c->pycache_prefix ||
        (c->check_hash_based_pycs && strcmp(c->check_hash_based_pycs, "default")))
        return FALSE;
    for (i = 0; i < c->nxoptions; i++) {
        if (!qp_xoption_is(c->xoptions[i], "utf8") &&
            !qp_xoption_is(c->xoptions[i], "frozen_modules"))
            return FALSE;
    }
    return TRUE;
}

/* The state of the base: like a start with -I -S -B (no environment, no
   site, no bytecode written at build time), a fixed hash seed, an empty
   posix.environ (QPConfig.detached), the stdlib of `home`/pylib, then the
   _io classes and the UTF-8 codec that the standard streams need.  The
   state that depends on the working directory (sys.path_importer_cache)
   is dropped and the garbage of the startup freed.  0, or -1 with an
   exception pending. */
static int qp_base_init(JSContext *ctx, const char *home)
{
    static char *argv[] = { (char *)"", NULL };
    QPConfig cfg;
    JSValue m, v;
    JSAtom name;

    memset(&cfg, 0, sizeof(cfg));
    cfg.hash_seed = "0";
    cfg.ignore_environment = 1;
    cfg.detached = 1;
    cfg.utf8_mode = 1;
    cfg.no_user_site = 1;
    cfg.no_site = 1;
    cfg.dont_write_bytecode = 1;
    cfg.int_max_str_digits = -1;
    cfg.lazy_imports = -1;
    cfg.argc = cfg.orig_argc = 1;
    cfg.argv = cfg.orig_argv = argv;
    cfg.home = home;
    if (qp_state_init(ctx, &cfg) < 0)
        return -1;
    if (qp_importlib_boot_core(ctx) < 0)
        return -1;
    /* the _io classes (_pyio) */
    m = qp_import_module(ctx, "_io");
    if (JS_IsException(m))
        return -1;
    name = JS_NewAtom(ctx, "TextIOWrapper");
    v = name == JS_ATOM_NULL ? JS_EXCEPTION : qp_getattr(ctx, m, name);
    JS_FreeAtom(ctx, name);
    JS_FreeValue(ctx, m);
    if (JS_IsException(v))
        return -1;
    JS_FreeValue(ctx, v);
    v = qp_codec_lookup(ctx, "utf-8");
    if (JS_IsException(v))
        return -1;
    JS_FreeValue(ctx, v);
    /* the finders of the sys.path entries of the build */
    name = JS_NewAtom(ctx, "path_importer_cache");
    if (name == JS_ATOM_NULL)
        return -1;
    v = qp_sys_get(ctx, name);
    JS_FreeAtom(ctx, name);
    if (qp_dict_check(ctx, v))
        qp_dictcore_clear(ctx, &QP_DICT(v)->core);
    JS_FreeValue(ctx, v);
    JS_RunGC(JS_GetRuntime(ctx));
    qp_run_pending_finalizers(ctx);
    qp_weakref_run_pending(ctx);
    return 0;
}

static void qp_base_put(DynBuf *d, const void *data, size_t n)
{
    dbuf_put(d, data, n);
}

/* `'\0'.join([sys._stdlib_dir] + the sorted __file__ of sys.modules)` as
   UTF-8 bytes */
static JSValue qp_base_file_list(JSContext *ctx)
{
    static const char src[] =
        "(lambda sys: '\\0'.join([sys._stdlib_dir] + sorted({m.__file__ for m in "
        "list(sys.modules.values()) if isinstance(getattr(m, '__file__', None), str)}))"
        ".encode('utf-8', 'surrogateescape'))(__import__('sys'))";
    JSValue d = qp_dict_new(ctx), r;

    if (JS_IsException(d))
        return d;
    r = qp_run_string(ctx, src, sizeof(src) - 1, "<base snapshot>", QP_INPUT_EVAL, d, d);
    JS_FreeValue(ctx, d);
    return r;
}

/* make dependency rule of the base: `target: files` */
static int qp_base_write_deps(const char *path, const char *list, size_t len)
{
    char *dpath = qp_sys_malloc(strlen(path) + 3);
    const char *p, *end = list + len, *e;
    FILE *f;

    if (!dpath)
        return -1;
    strcpy(dpath, path);
    strcat(dpath, ".d");
    f = fopen(dpath, "w");
    qp_sys_free(dpath);
    if (!f)
        return -1;
    fprintf(f, "%s:", path);
    p = memchr(list, '\0', len);   /* skip the stdlib entry */
    p = p ? p + 1 : end;
    for (; p < end; p = e + 1) {
        e = memchr(p, '\0', end - p);
        if (!e)
            e = end;
        fputs(" \\\n ", f);
        for (; p < e; p++) {
            if (*p == ' ' || *p == '#' || *p == '$')
                fputc(*p == '$' ? '$' : '\\', f);
            fputc(*p, f);
        }
    }
    fputc('\n', f);
    return fclose(f) == 0 ? 0 : -1;
}

/* `qpy --write-base-snapshot FILE DIR`: write the base snapshot of the
   qpy in DIR (whose stdlib is DIR/pylib) and the make dependencies of
   FILE on its source files (FILE.d).  Returns the exit status. */
int qp_write_base_snapshot(const char *path, const char *dir)
{
    char home[PATH_MAX];
    JSRuntime *rt;
    JSContext *ctx;
    JSValue list = JS_UNDEFINED;
    uint8_t *snap = NULL;
    size_t snap_len = 0, list_len, n;
    const uint8_t *list_data;
    const char *p, *end, *e;
    DynBuf d;
    uint32_t nfiles = 0, u32;
    uint64_t u64;
    struct stat st;
    FILE *f;
    int i, ret = 1;

    if (!realpath(dir, home)) {
        perror(dir);
        return 1;
    }
    /* every signal with its default action, as qp_drepl_init (the base
       must not record the dispositions of the build) */
    for (i = 1; i < QP_MAIN_NSIG; i++) {
        struct sigaction sa;
        if (sigaction(i, NULL, &sa) == 0 && sa.sa_handler == SIG_IGN)
            signal(i, SIG_DFL);
    }
    rt = JS_NewRuntime();
    if (!rt)
        return 1;
    qp_main_set_stack(rt);
    ctx = JS_NewContext(rt);
    if (!ctx) {
        JS_FreeRuntime(rt);
        return 1;
    }
    dbuf_init(&d);
    if (qp_base_init(ctx, home) < 0)
        goto fail;
    snap = JS_WriteSnapshot(ctx, &snap_len, NULL, NULL, 0, 0);
    if (!snap)
        goto fail;
    list = qp_base_file_list(ctx);
    if (JS_IsException(list))
        goto fail;
    list_data = qp_bytes_data(list, (Py_ssize_t *)&list_len);
    if (!list_data)
        goto fail;

    qp_base_put(&d, QP_BASE_MAGIC, QP_BASE_MAGIC_LEN);
    qp_base_put(&d, &nfiles, 4);   /* patched below */
    end = (const char *)list_data + list_len;
    e = memchr(list_data, '\0', list_len);
    if (!e)
        e = end;
    for (p = e + 1; p < end; p = e + 1) {
        char fpath[PATH_MAX];
        int64_t v;
        e = memchr(p, '\0', end - p);
        if (!e)
            e = end;
        n = e - p;
        if (n >= sizeof(fpath))
            continue;
        memcpy(fpath, p, n);
        fpath[n] = '\0';
        if (stat(fpath, &st) < 0) {
            perror(fpath);
            goto done;
        }
        u32 = (uint32_t)n;
        qp_base_put(&d, &u32, 4);
        qp_base_put(&d, fpath, n);
        v = (int64_t)QP_BASE_ST_MTIM(st).tv_sec;
        qp_base_put(&d, &v, 8);
        v = (int64_t)QP_BASE_ST_MTIM(st).tv_nsec;
        qp_base_put(&d, &v, 8);
        v = (int64_t)st.st_size;
        qp_base_put(&d, &v, 8);
        nfiles++;
    }
    p = memchr(list_data, '\0', list_len);
    u32 = (uint32_t)(p ? (size_t)(p - (const char *)list_data) : list_len);
    qp_base_put(&d, &u32, 4);
    qp_base_put(&d, list_data, u32);
    u64 = snap_len;
    qp_base_put(&d, &u64, 8);
    qp_base_put(&d, snap, snap_len);
    if (d.error) {
        fprintf(stderr, "qpy: %s: out of memory\n", path);
        goto done;
    }
    memcpy(d.buf + QP_BASE_MAGIC_LEN, &nfiles, 4);
    f = fopen(path, "wb");
    if (!f || fwrite(d.buf, 1, d.size, f) != d.size || fclose(f) != 0) {
        perror(path);
        goto done;
    }
    if (qp_base_write_deps(path, (const char *)list_data, list_len) < 0) {
        perror(path);
        goto done;
    }
    ret = 0;
    goto done;
 fail:
    qp_handle_uncaught(ctx);
 done:
    dbuf_free(&d);
    JS_FreeValue(ctx, list);
    if (snap)
        js_free(ctx, snap);
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
    return ret;
}

/* The process half of the start of a state restored from the base (see
   above).  0, -1 with an exception pending (reported like an error of a
   normal start), or 1 when the base does not apply (the stdlib of this
   process is not the base's). */
static int qp_main_attach_base(JSContext *ctx, QPMain *m, const QPBaseBlob *b,
                               const uint8_t *ignored)
{
    QPState *s = QP_STATE(ctx);
    const uint8_t *data;
    Py_ssize_t len;
    JSValue v, enc;
    JSAtom name;
    int r;

    s->config = m->cfg;
    js_random_init(ctx);
    js_performance_init(ctx);
    if (qp_importlib_boot_paths(ctx) < 0)
        return -1;
    name = JS_NewAtom(ctx, "_stdlib_dir");
    if (name == JS_ATOM_NULL)
        return -1;
    v = qp_sys_get(ctx, name);
    JS_FreeAtom(ctx, name);
    if (!qp_str_check(ctx, v)) {
        JS_FreeValue(ctx, v);
        return 1;
    }
    enc = qp_str_encode(ctx, v, "utf-8", "surrogateescape");
    JS_FreeValue(ctx, v);
    if (JS_IsException(enc))
        return -1;
    data = qp_bytes_data(enc, &len);
    r = !data || (size_t)len != b->stdlib_len || memcmp(data, b->stdlib, len);
    JS_FreeValue(ctx, enc);
    if (r)
        return 1;
    if (qp_sys_set_config_attrs(ctx, s->sys_dict, TRUE) < 0)
        return -1;
    qp_signal_attach(ctx);
    qp_signal_apply_inherited(ctx, ignored, QP_MAIN_NSIG);
    qp_os_drepl_environ(ctx, TRUE);
    name = JS_NewAtom(ctx, "time");
    if (name == JS_ATOM_NULL)
        return -1;
    r = qp_dict_get_item_atom(ctx, s->modules, name, &v);
    JS_FreeAtom(ctx, name);
    if (r < 0)
        return -1;
    if (r > 0) {
        r = qp_time_init_timezone(ctx, v);
        JS_FreeValue(ctx, v);
        if (r < 0)
            return -1;
    }
    return qp_importlib_boot_process(ctx);
}

/* Start from the base snapshot: 1 with *pctx the restored context, 2
   likewise with an exception pending (the start failed like a normal one
   would), 0 when the base is not used (rt is untouched), -1 when the
   base did not apply after rt was used (rt must be freed; the signal
   dispositions are restored). */
static int qp_main_restore_base(QPMain *m, JSRuntime *rt, JSContext **pctx)
{
    struct sigaction saved[QP_MAIN_NSIG];
    uint8_t ignored[QP_MAIN_NSIG], have[QP_MAIN_NSIG];
    uint8_t key[16];
    QPBaseBlob b;
    JSSnapshotResult res;
    char errbuf[256];
    JSContext *ctx;
    qp_error err;
    int i, r;

    if (!qp_main_base_eligible(m) || qp_base_parse(&b) < 0 || !qp_base_files_current(&b))
        return 0;
    memset(&err, 0, sizeof(err));
    if (qp_hash_key_init(m->cfg.hash_seed, key, &err) < 0) {
        /* the normal start reports it */
        qp_error_clear(&err);
        return 0;
    }
    for (i = 1; i < QP_MAIN_NSIG; i++) {
        have[i] = sigaction(i, NULL, &saved[i]) == 0;
        ignored[i] = have[i] && saved[i].sa_handler == SIG_IGN;
    }
    ignored[0] = 0;
    errbuf[0] = '\0';
    rt->py_snap_hash_key = key;
    r = JS_ReadSnapshot(rt, b.snap, b.snap_len, NULL, 0, NULL, &res, errbuf, sizeof(errbuf));
    rt->py_snap_hash_key = NULL;
    if (r < 0)
        goto fail;
    if (res.context_count != 1 || !res.contexts[0]->py) {
        for (i = 0; i < res.context_count; i++)
            JS_FreeContext(res.contexts[i]);
        JS_FreeSnapshotResult(rt, &res);
        goto fail;
    }
    ctx = res.contexts[0];
    JS_FreeSnapshotResult(rt, &res);
    r = qp_main_attach_base(ctx, m, &b, ignored);
    if (r > 0) {
        JS_FreeContext(ctx);
        goto fail;
    }
    *pctx = ctx;
    return r < 0 ? 2 : 1;
 fail:
    for (i = 1; i < QP_MAIN_NSIG; i++) {
        if (have[i])
            sigaction(i, &saved[i], NULL);
    }
    return -1;
}

int qp_main(int argc, char **argv)
{
    QPMain m;
    JSRuntime *rt;
    JSContext *ctx;
    JSValue main_module;
    int exitcode = 0, rc, nthreads;

    memset(&m, 0, sizeof(m));
    /* _Py_SetLocaleFromEnv(LC_CTYPE) (pymain_init) */
    setlocale(LC_CTYPE, "");
    rc = qp_main_parse(&m, argc, argv);
    if (rc >= 0) {
        qp_main_free(&m);
        return rc;
    }

    rt = JS_NewRuntime();
    if (!rt) {
        fprintf(stderr, "Fatal Python error: init: cannot allocate the runtime\n");
        qp_main_free(&m);
        return 1;
    }
    qp_main_set_stack(rt);
    ctx = NULL;
    rc = qp_main_restore_base(&m, rt, &ctx);
    if (rc < 0) {
        /* a fresh runtime for the normal start */
        JS_FreeRuntime(rt);
        rt = JS_NewRuntime();
        if (!rt) {
            fprintf(stderr, "Fatal Python error: init: cannot allocate the runtime\n");
            qp_main_free(&m);
            return 1;
        }
        qp_main_set_stack(rt);
    }
    if (rc == 2) {
        exitcode = qp_main_err_print(ctx, &m);
        goto done;
    }
    if (rc <= 0) {
        ctx = JS_NewContext(rt);
        if (!ctx) {
            fprintf(stderr, "Fatal Python error: init: cannot allocate the context\n");
            JS_FreeRuntime(rt);
            qp_main_free(&m);
            return 1;
        }
        if (qp_state_init(ctx, &m.cfg) < 0) {
            fprintf(stderr, "Fatal Python error: init: Python state initialization failed\n");
            JS_FreeContext(ctx);
            JS_FreeRuntime(rt);
            qp_main_free(&m);
            return 1;
        }
#ifdef QP_IMPORTLIB_BOOT
        if (qp_importlib_boot(ctx) < 0) {
            exitcode = qp_main_err_print(ctx, &m);
            goto done;
        }
#endif
    }
    if (m.cfg.tracemalloc > 0 && qp_main_start_tracemalloc(ctx, m.cfg.tracemalloc) < 0) {
        exitcode = qp_main_err_print(ctx, &m);
        goto done;
    }
    main_module = qp_run_main_module(ctx);
    if (JS_IsException(main_module)) {
        exitcode = qp_main_err_print(ctx, &m);
        goto done;
    }
    JS_FreeValue(ctx, main_module);
    if (m.cfg.sys_path_0 && qp_main_add_path0(ctx, m.cfg.sys_path_0) < 0) {
        exitcode = qp_main_err_print(ctx, &m);
        goto done;
    }

    qp_main_header(&m);
    if (m.cfg.run_command)
        exitcode = qp_main_run_command(ctx, &m);
    else if (m.cfg.run_module)
        exitcode = qp_main_run_module(ctx, &m, m.cfg.run_module);
    else if (m.cfg.run_filename)
        exitcode = qp_main_run_file(ctx, &m);
    else
        exitcode = qp_main_run_stdin(ctx, &m);
    qp_main_repl(ctx, &m, &exitcode);

 done:
    /* Py_FinalizeEx (a failed flush of the standard streams gives status
       120); the remaining (daemon) threads hang when they try to take the
       GIL */
    if (qp_finalize(ctx, &nthreads) < 0)
        exitcode = 120;
    if (m.kb)
        exitcode = 130;
    /* daemon threads still reference the runtime: no teardown (CPython
       does not free their thread states either) */
    if (nthreads > 0)
        return exitcode;
    /* like qjs: release everything (the state keeps pointers into m.cfg),
       so that leaks and teardown bugs are visible */
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
    qp_main_free(&m);
    return exitcode;
}

/* ---- durable REPL (qjs --drepl FILE --python) ------------------------- */

/* qjs keeps the line editor in its own runtime and runs the inputs in the
   session context through these functions.  The session is saved with
   JS_WriteSnapshot() as a delta of its base, a new session, which qjs
   rebuilds in each process: the base must be the same bytes in every
   process of the build, so qp_drepl_init() does not depend on the
   environment. */

/* "<stdin-N>" counter of the session: a hidden property of the global
   object, so that it survives the restarts like the sources registered
   for the tracebacks */
#define QP_DREPL_COUNT "\x01qp_drepl_count"

/* A new session: Python like the interactive `python`, with options that
   make the state reproducible: the hash seed of the session file,
   ignore_environment (PYTHONPATH, PYTHONHOME... would change the base),
   an empty posix.environ (qp_drepl_attach() fills it), the UTF-8 mode
   (the encodings would otherwise depend on the locale), no user site,
   unbuffered standard streams (their buffering would otherwise depend on
   isatty()), sys.argv == [''] and every signal recorded with its default
   action (a signal ignored by the parent process is ignored again
   afterwards, except SIGINT: the state says SIG_DFL).  The state that
   depends on the working directory (sys.path_importer_cache) is dropped
   and the garbage of the startup freed.  Returns 0, or -1 after printing
   the error. */
int qp_drepl_init(JSContext *ctx, const char *argv0, uint32_t hash_seed)
{
    static char *argv[] = { (char *)"", NULL };
    static char seed[16];
    static char *exe;
#ifdef NSIG
    enum { nsig = NSIG };
#else
    enum { nsig = 65 };
#endif
    BOOL ignored[nsig];
    struct sigaction sa;
    QPConfig cfg;
    JSValue m;
    int i, ret = -1;

    memset(&cfg, 0, sizeof(cfg));
    snprintf(seed, sizeof(seed), "%u", (unsigned)hash_seed);
    if (!exe)
        exe = qp_main_executable(argv0);
    cfg.executable = exe ? exe : argv0;
    cfg.hash_seed = seed;
    cfg.ignore_environment = 1;
    cfg.detached = 1;
    cfg.utf8_mode = 1;
    cfg.no_user_site = 1;
    cfg.unbuffered = 1;
    cfg.dont_write_bytecode = 1;
    cfg.int_max_str_digits = -1;
    cfg.lazy_imports = -1;
    cfg.argc = cfg.orig_argc = 1;
    cfg.argv = cfg.orig_argv = argv;
    cfg.sys_path_0 = "";

    for (i = 1; i < nsig; i++) {
        ignored[i] = sigaction(i, NULL, &sa) == 0 && sa.sa_handler == SIG_IGN;
        if (ignored[i])
            signal(i, SIG_DFL);
    }
    if (qp_state_init(ctx, &cfg) < 0) {
        fprintf(stderr, "Fatal Python error: init: Python state initialization failed\n");
        goto done;
    }
    /* the base of a session is rebuilt in every process and must be the
       same bytes: modules are always compiled from source, no bytecode
       cache is read or written (sys.implementation.cache_tag = None) */
    {
        JSAtom a = JS_NewAtom(ctx, "implementation"), tag;
        JSValue impl = a == JS_ATOM_NULL ? JS_EXCEPTION : qp_sys_get(ctx, a);
        int r = -1;
        JS_FreeAtom(ctx, a);
        if (!JS_IsException(impl) && !QP_IS_NULL(impl)) {
            tag = JS_NewAtom(ctx, "cache_tag");
            if (tag != JS_ATOM_NULL) {
                r = qp_setattr(ctx, impl, tag, JS_UNDEFINED);
                JS_FreeAtom(ctx, tag);
            }
        }
        if (!JS_IsException(impl))
            JS_FreeValue(ctx, impl);
        if (r < 0)
            goto fail;
    }
#ifdef QP_IMPORTLIB_BOOT
    if (qp_importlib_boot(ctx) < 0)
        goto fail;
#endif
    /* install_signal_handlers: SIGINT raises KeyboardInterrupt */
    m = qp_import_module(ctx, "_signal");
    if (JS_IsException(m))
        goto fail;
    JS_FreeValue(ctx, m);
    m = qp_run_main_module(ctx);
    if (JS_IsException(m))
        goto fail;
    JS_FreeValue(ctx, m);
    if (qp_main_add_path0(ctx, cfg.sys_path_0) < 0)
        goto fail;
    qp_repl_default_prompt(ctx, QP_ATOM(ctx, ps1), ">>> ");
    qp_repl_default_prompt(ctx, QP_ATOM(ctx, ps2), "... ");
    if (JS_DefinePropertyValueStr(ctx, ctx->global_obj, QP_DREPL_COUNT, JS_NewInt32(ctx, 0),
                                  JS_PROP_CONFIGURABLE | JS_PROP_WRITABLE) < 0)
        goto fail;
    /* the finders of sys.path entries: '' is the working directory */
    {
        JSAtom name = JS_NewAtom(ctx, "path_importer_cache");
        if (name == JS_ATOM_NULL)
            goto fail;
        m = qp_sys_get(ctx, name);
        JS_FreeAtom(ctx, name);
        if (qp_dict_check(ctx, m))
            qp_dictcore_clear(ctx, &QP_DICT(m)->core);
        JS_FreeValue(ctx, m);
    }
    /* the garbage of the startup: its amount depends on the process (not
       gc.collect(), whose statistics are part of the state) */
    JS_RunGC(JS_GetRuntime(ctx));
    qp_run_pending_finalizers(ctx);
    qp_weakref_run_pending(ctx);
    ret = 0;
    goto done;
 fail:
    qp_handle_uncaught(ctx);
 done:
    for (i = 1; i < nsig; i++) {
        if (ignored[i] && i != SIGINT)
            signal(i, SIG_IGN);
    }
    return ret;
}

/* The session ctx, new (after writing its base) or restored: -1 if it has
   no Python state (not a Python session), else 0 after making it the
   context of the signal handlers and filling posix.environ from the
   environment of the process. */
int qp_drepl_attach(JSContext *ctx)
{
    if (!ctx->py)
        return -1;
    qp_signal_attach(ctx);
    qp_os_drepl_environ(ctx, TRUE);
    /* a durable session does not end with the process: no Py_FinalizeEx
       (atexit...) when qjs frees the runtime */
    QP_STATE(ctx)->finalized = 1;
    return 0;
}

/* posix.environ of the session ctx: emptied before the session is saved
   ('fill' FALSE), so that the session file never holds the values of the
   environment, then filled again ('fill' TRUE) */
void qp_drepl_environ(JSContext *ctx, int fill)
{
    qp_os_drepl_environ(ctx, fill);
}

/* Install the C handler of SIGINT of the session (Ctrl-C raises
   KeyboardInterrupt in the running input) and return the previous one. */
void (*qp_drepl_signals(JSContext *ctx))(int)
{
    return qp_signal_attach(ctx);
}

/* the stack size of `python` for the session runtime */
void qp_drepl_set_stack(JSRuntime *rt)
{
    qp_main_set_stack(rt);
}

/* pymain_header */
void qp_drepl_banner(char *buf, size_t size)
{
    snprintf(buf, size, "Python " QP_PY_VERSION " (main) [dquickts " CONFIG_VERSION "] on %s\n"
             "Type \"help\", \"copyright\", \"credits\" or \"license\" for more information.\n",
#if defined(__APPLE__)
             "darwin"
#elif defined(__linux__)
             "linux"
#else
             "unknown"
#endif
             );
}

/* str(sys.ps1), or str(sys.ps2) if 'more', as UTF-8 (free with
   JS_FreeCString(ctx, ...)); NULL on failure */
const char *qp_drepl_prompt(JSContext *ctx, int more)
{
    DynBuf buf;
    JSValue str;
    const char *s;

    dbuf_init(&buf);
    qp_repl_prompt(ctx, more ? QP_ATOM(ctx, ps2) : QP_ATOM(ctx, ps1), &buf);
    if (dbuf_error(&buf)) {
        dbuf_free(&buf);
        return NULL;
    }
    str = JS_NewStringLen(ctx, buf.buf ? (const char *)buf.buf : "", buf.size);
    dbuf_free(&buf);
    if (JS_IsException(str)) {
        JS_FreeValue(ctx, JS_GetException(ctx));
        return NULL;
    }
    s = JS_ToCString(ctx, str);
    JS_FreeValue(ctx, str);
    if (!s)
        JS_FreeValue(ctx, JS_GetException(ctx));
    return s;
}

/* codeop's decision on the accumulated lines src (src[len] == '\0'): 1 if
   the input is complete (or has an error that more lines cannot fix), 0
   if it needs more lines */
int qp_drepl_input_complete(JSContext *ctx, const char *src, size_t len)
{
    DynBuf buf;
    int rc;

    dbuf_init(&buf);
    dbuf_put(&buf, (const uint8_t *)src, len);
    dbuf_putc(&buf, '\0');
    if (dbuf_error(&buf)) {
        dbuf_free(&buf);
        return 1;   /* the compilation reports the MemoryError */
    }
    buf.size--;
    rc = qp_repl_input_complete(ctx, &buf);
    dbuf_free(&buf);
    return rc;
}

/* Wait for y, a value yielded by the top-level coroutine (asyncio.Task
   __step rules).  Sets *pvalue to the value to send: None after a bare
   yield (once the pending jobs ran), the result of a JS promise once the
   jobs settled it.  Returns 1 if *pvalue is an exception to throw
   instead: the rejection reason, or RuntimeError for a promise that no
   job settles or for any other value.  -1 if a job failed (exception
   pending). */
static int qp_drepl_wait(JSContext *ctx, JSValueConst y, JSValue *pvalue)
{
    JSContext *ctx1;
    int state, r;

    *pvalue = JS_UNDEFINED;
    if (JS_IsUndefined(y) || JS_IsNull(y)) {
        while ((r = JS_ExecutePendingJob(ctx->rt, &ctx1)) > 0)
            continue;
        return r < 0 ? -1 : 0;
    }
    /* JS_PromiseState / JS_PromiseResult take a borrowed (non-consumed) value */
    if ((int)JS_PromiseState(ctx, (JSValue)y) < 0) {
        qp_raise_runtime_error(ctx, "Task got bad yield: %R", y);
    } else {
        while ((state = JS_PromiseState(ctx, (JSValue)y)) == JS_PROMISE_PENDING &&
               (r = JS_ExecutePendingJob(ctx->rt, &ctx1)) != 0) {
            if (r < 0)
                return -1;
        }
        if (state == JS_PROMISE_FULFILLED) {
            *pvalue = JS_PromiseResult(ctx, (JSValue)y);
            return 0;
        }
        if (state == JS_PROMISE_REJECTED) {
            *pvalue = qp_exc_normalize(ctx, JS_PromiseResult(ctx, (JSValue)y));
            return 1;
        }
        qp_raise_runtime_error(ctx, "the awaited promise is never settled (no pending job)");
    }
    *pvalue = qp_exc_fetch(ctx);
    return 1;
}

/* Run the coroutine of an input with top-level await (consumed) to its
   end with the job queue: its result, or JS_EXCEPTION */
static JSValue qp_drepl_await(JSContext *ctx, JSValue coro)
{
    JSValue value = JS_UNDEFINED, y = JS_UNDEFINED, exc;
    int r, is_throw = 0;

    for (;;) {
        if (!is_throw) {
            r = qp_iter_send(ctx, coro, value, &y);
        } else {
            y = qp_call_method(ctx, coro, QP_ATOM(ctx, throw), (JSValueConst *)&value, 1,
                               QP_NULL);
            if (!JS_IsException(y)) {
                r = QP_SEND_NEXT;
            } else if (qp_exc_pending_matches(ctx, QP_EXC(ctx, StopIteration))) {
                exc = qp_exc_fetch(ctx);
                y = qp_exc_stopiteration_value(ctx, exc);
                JS_FreeValue(ctx, exc);
                r = QP_SEND_RETURN;
            } else {
                r = QP_SEND_ERROR;
            }
        }
        JS_FreeValue(ctx, value);
        if (r != QP_SEND_NEXT)
            break;
        is_throw = qp_drepl_wait(ctx, y, &value);
        JS_FreeValue(ctx, y);
        if (is_throw < 0) {
            r = QP_SEND_ERROR;
            break;
        }
    }
    JS_FreeValue(ctx, coro);
    return r == QP_SEND_RETURN ? y : JS_EXCEPTION;
}

/* Run the complete input src (src[len] == '\0') like the interactive
   loop, with top-level await (PyCF_ALLOW_TOP_LEVEL_AWAIT: the coroutine
   runs to its end with the job queue), then the pending jobs.  The
   results go to sys.displayhook, the exceptions to sys.excepthook.
   Returns 0, 1 if an exception was reported, or 2 with *pstatus for
   SystemExit (the exit status of `python`; the message of a code that is
   not an int is printed). */
int qp_drepl_exec(JSContext *ctx, const char *src, size_t len, int *pstatus)
{
    JSContext *ctx1;
    JSValue v, res;
    int count = 0, kb, r;

    v = JS_GetPropertyStr(ctx, ctx->global_obj, QP_DREPL_COUNT);
    if (JS_VALUE_GET_TAG(v) == JS_TAG_INT)
        count = JS_VALUE_GET_INT(v);
    JS_FreeValue(ctx, v);
    res = qp_repl_exec(ctx, src, len, &count, QP_PF_IGNORE_COOKIE | QP_PF_TOP_LEVEL_AWAIT);
    if (JS_DefinePropertyValueStr(ctx, ctx->global_obj, QP_DREPL_COUNT, JS_NewInt32(ctx, count),
                                  JS_PROP_CONFIGURABLE | JS_PROP_WRITABLE) < 0) {
        JS_FreeValue(ctx, res);
        res = JS_EXCEPTION;
    }
    if (!JS_IsException(res) && qp_coro_check(res))
        res = qp_drepl_await(ctx, res);
    if (!JS_IsException(res)) {
        JS_FreeValue(ctx, res);
        while ((r = JS_ExecutePendingJob(ctx->rt, &ctx1)) > 0)
            continue;
        if (r < 0)
            res = JS_EXCEPTION;
    }
    /* the __del__ methods and weakref callbacks queued by the collections
       of the input run now, as CPython runs them during the collection
       (the eval breaker would run them at the next input): the session
       is saved with an empty queue */
    qp_run_pending_finalizers(ctx);
    qp_weakref_run_pending(ctx);
    qp_run_flush_io(ctx);
    if (!JS_IsException(res))
        return 0;
    r = 1;
    if (!QP_STATE(ctx)->config.inspect && qp_exc_pending_matches(ctx, QP_EXC(ctx, SystemExit)))
        r = 2;
    *pstatus = qp_err_print_uncaught(ctx, &kb);
    qp_run_flush_io(ctx);
    return r;
}

/* Completion of the word 'prefix' at the end of a line: the attributes of
   the value of the dotted name 'expr' (evaluated in __main__ like
   rlcompleter does), or for an empty 'expr' the keywords, the globals of
   __main__ and the builtins.  Sorted and separated by newlines (free with
   JS_FreeCString(ctx, ...)), NULL on failure. */
const char *qp_drepl_names(JSContext *ctx, const char *expr, const char *prefix)
{
    static const char code[] =
        "r = []\n"
        "try:\n"
        "    if expr:\n"
        "        if all(p.isidentifier() for p in expr.split('.')):\n"
        "            r = dir(eval(expr, ns))\n"
        "    else:\n"
        "        r = [*kw.split(), *ns, *dir(__builtins__)]\n"
        "except BaseException:\n"
        "    pass\n"
        "r = '\\n'.join(sorted({n for n in r if n.startswith(prefix) and\n"
        "                       (prefix[:1] == '_' or n[:1] != '_')}))\n";
    static const char keywords[] =
        "False None True and as assert async await break class continue def del "
        "elif else except finally for from global if import in is lambda nonlocal "
        "not or pass raise return try while with yield";
    static const char *const names[] = { "expr", "prefix", "kw" };
    const char *values[3];
    const char *s = NULL;
    JSValue main_module, d = JS_UNDEFINED, ns = JS_UNDEFINED, k, v, res;
    int i, rc;

    values[0] = expr;
    values[1] = prefix;
    values[2] = keywords;
    main_module = qp_run_main_module(ctx);
    if (JS_IsException(main_module))
        goto done;
    ns = qp_run_main_dict(ctx, main_module);
    JS_FreeValue(ctx, main_module);
    if (JS_IsException(ns))
        goto done;
    d = qp_dict_new(ctx);
    if (JS_IsException(d))
        goto done;
    k = qp_str_from_cstr(ctx, "ns");
    if (JS_IsException(k))
        goto done;
    rc = qp_dict_set_item(ctx, d, k, ns);
    JS_FreeValue(ctx, k);
    if (rc < 0)
        goto done;
    for (i = 0; i < 3; i++) {
        k = qp_str_from_cstr(ctx, names[i]);
        v = qp_str_from_cstr(ctx, values[i]);
        rc = (JS_IsException(k) || JS_IsException(v)) ? -1 : qp_dict_set_item(ctx, d, k, v);
        JS_FreeValue(ctx, k);
        JS_FreeValue(ctx, v);
        if (rc < 0)
            goto done;
    }
    res = qp_run_string(ctx, code, sizeof(code) - 1, "<completion>", QP_INPUT_FILE, d, d);
    if (JS_IsException(res))
        goto done;
    JS_FreeValue(ctx, res);
    k = qp_str_from_cstr(ctx, "r");
    if (JS_IsException(k))
        goto done;
    rc = qp_dict_get_item(ctx, d, k, &v);
    JS_FreeValue(ctx, k);
    if (rc > 0) {
        s = qp_str_utf8(ctx, v, NULL);
        JS_FreeValue(ctx, v);
    }
 done:
    if (JS_HasException(ctx))
        JS_FreeValue(ctx, JS_GetException(ctx));
    JS_FreeValue(ctx, d);
    JS_FreeValue(ctx, ns);
    return s;
}
