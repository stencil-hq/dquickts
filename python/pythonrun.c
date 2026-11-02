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

#include "quickpy.h"
#include "pythonrun_usage.h"

/* provided by sysmodule.c (k) and errors.c (i) */
static int qp_sys_flush_std_files(JSContext *ctx);
static int qp_err_print_uncaught(JSContext *ctx, int *pkbint);
static int qp_linecache_register(JSContext *ctx, const char *filename, const char *src, size_t len);

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
    JSValue bc;

    memset(&err, 0, sizeof(err));
    bc = qp_compile_source(ctx, src, len, filename, mode, flags, -1, &err);
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

/* PyOS_StdioReadline: prompt on stderr, one line (without '\n') appended
   to line.  1 = line read, 0 = EOF before any character. */
static int qp_repl_readline(JSContext *ctx, JSAtom prompt_name, DynBuf *line)
{
    DynBuf prompt;
    int c, got = 0;

    dbuf_init(&prompt);
    qp_repl_prompt(ctx, prompt_name, &prompt);
    fwrite(prompt.buf ? (const char *)prompt.buf : "", 1, prompt.size, stderr);
    fflush(stderr);
    dbuf_free(&prompt);
    while ((c = getc(stdin)) != EOF) {
        got = 1;
        if (c == '\n')
            return 1;
        dbuf_putc(line, c);
    }
    clearerr(stdin);
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
    rc = qp_parse(ctx, arena, src, len, "<stdin>", QP_INPUT_SINGLE,
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
   compile as "<stdin-N>" (source registered for tracebacks), run in
   __main__ with print_expr.  0 or -1 with exception pending. */
static int qp_repl_run_one(JSContext *ctx, const char *src, size_t len, int *pcount)
{
    char filename[32];
    qp_error err;
    qp_arena *arena;
    mod_ty mod;
    JSValue main_module, d, bc, res;
    const int flags = QP_PF_IGNORE_COOKIE;

    memset(&err, 0, sizeof(err));
    arena = qp_arena_new(ctx, &err);
    if (!arena) {
        qp_error_clear(&err);
        qp_raise_memory_error(ctx);
        return -1;
    }
    if (qp_parse(ctx, arena, src, len, "<stdin>", QP_INPUT_SINGLE, flags, &mod, &err) < 0) {
        qp_arena_free(arena);
        qp_raise_qp_error(ctx, &err, "<stdin>");
        qp_error_clear(&err);
        return -1;
    }
    snprintf(filename, sizeof(filename), "<stdin-%d>", (*pcount)++);
    if (qp_linecache_register(ctx, filename, src, len) < 0) {
        qp_arena_free(arena);
        return -1;
    }
    bc = qp_compile_ast(ctx, mod, filename, QP_INPUT_SINGLE, flags, -1, arena, &err);
    qp_arena_free(arena);
    if (JS_IsException(bc)) {
        if (err.kind != QP_ERR_NONE)
            qp_raise_qp_error(ctx, &err, filename);
        qp_error_clear(&err);
        return -1;
    }
    qp_error_clear(&err);
    main_module = qp_run_main_module(ctx);
    if (JS_IsException(main_module)) {
        JS_FreeValue(ctx, bc);
        return -1;
    }
    d = qp_run_main_dict(ctx, main_module);
    if (JS_IsException(d)) {
        JS_FreeValue(ctx, bc);
        JS_FreeValue(ctx, main_module);
        return -1;
    }
    res = qp_run_code(ctx, bc, d, d);
    JS_FreeValue(ctx, d);
    JS_FreeValue(ctx, main_module);
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
    fprintf(stderr, "Fatal Python error: %s: %s\nPython runtime state: preinitialized\n\n", func, msg);
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

/* Absolute program path (sys.executable): argv[0] if it holds a '/',
   else the first match in $PATH. */
static char *qp_main_executable(const char *argv0)
{
    char buf[PATH_MAX], cand[PATH_MAX];
    const char *path, *p, *q;
    char *res;
    size_t n;

    if (!argv0 || !*argv0)
        return NULL;
    if (strchr(argv0, '/')) {
        if (realpath(argv0, buf))
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
                argv0 = realpath(cand, buf) ? buf : cand;
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
    if (!cfg->run_command && !cfg->run_module && g.optind < argc && strcmp(argv[g.optind], "-") != 0)
        cfg->run_filename = argv[g.optind];
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
    qp_sys_free(m->cfg.xoptions);
    qp_sys_free(m->cfg.warnoptions);
}

int qp_main(int argc, char **argv)
{
    QPMain m;
    JSRuntime *rt;
    JSContext *ctx;
    JSValue main_module;
    int exitcode = 0, rc;

    memset(&m, 0, sizeof(m));
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
    ctx = JS_NewContext(rt);
    if (!ctx) {
        fprintf(stderr, "Fatal Python error: init: cannot allocate the context\n");
        qp_main_free(&m);
        return 1;
    }
    if (qp_state_init(ctx, &m.cfg) < 0) {
        fprintf(stderr, "Fatal Python error: init: Python state initialization failed\n");
        qp_main_free(&m);
        return 1;
    }
#ifdef QP_IMPORTLIB_BOOT
    if (qp_importlib_boot(ctx) < 0) {
        exitcode = qp_main_err_print(ctx, &m);
        goto done;
    }
#endif
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
    /* Py_FinalizeEx order: threads, atexit, then the standard streams
       (a failed flush gives status 120) */
    qp_main_thread_shutdown(ctx);
    qp_atexit_run(ctx);
    if (qp_sys_flush_std_files(ctx) < 0)
        exitcode = 120;
    fflush(stdout);
    fflush(stderr);
    if (m.kb)
        exitcode = 130;
    qp_main_free(&m);
    return exitcode;
}
