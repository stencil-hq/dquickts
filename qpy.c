/*
 * qpy: Python 3.15 front end of dquickts.
 *
 *   qpy [option] ... [-c cmd | -m mod | file | -] [arg] ...
 *
 * The CPython command line (Modules/main.c, Python/initconfig.c) is
 * implemented by qp_main() (python/pythonrun.c) so that `qjs file.py` and
 * `qjs --python` share it.  qpy gives it the JS environment of
 * quickjs-libc (qpy_host: the std/os/bjson modules, the global timers and
 * the event loop after __main__).  Two extra options are kept for the test
 * harness and the build:
 *
 *   qpy --dump-ast-json-batch   serve the JSON-lines AST protocol on
 *                               stdin/stdout (see python/quickpy.h)
 *   qpy --write-frozen-modules FILE DIR
 *                               write the frozen modules of the qpy of
 *                               directory DIR (qp_write_frozen_modules),
 *                               which qpy-base.c embeds
 *   qpy --write-base-snapshot FILE DIR [FROZEN]
 *                               write the base snapshot of the qpy of
 *                               directory DIR (qp_write_base_snapshot)
 *                               made with the frozen modules of the file
 *                               FROZEN, which qpy-base.c embeds
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "quickjs.h"
#include "quickjs-libc.h"
#include "python/quickpy.h"
#ifdef CONFIG_PYTHON_CAPI
#include "capi/qpcapi.h"
#endif

/* qpy-base.c: the embedded base snapshot and frozen modules (NULL in
   qpy-nobase) */
const uint8_t *qpy_base_snapshot(size_t *psize);
const uint8_t *qpy_frozen_modules(size_t *psize);

/* the content of the file `path` (malloc'd, kept for the process) */
static uint8_t *read_whole_file(const char *path, size_t *psize)
{
    FILE *f = fopen(path, "rb");
    uint8_t *buf = NULL;
    long n;

    if (!f || fseek(f, 0, SEEK_END) < 0 || (n = ftell(f)) < 0 || fseek(f, 0, SEEK_SET) < 0 ||
        !(buf = malloc(n ? n : 1)) || fread(buf, 1, n, f) != (size_t)n) {
        perror(path);
        free(buf);
        if (f)
            fclose(f);
        return NULL;
    }
    fclose(f);
    *psize = (size_t)n;
    return buf;
}

/* the context of the workers started from the Python program */
static JSContext *qpy_new_worker_context(JSRuntime *rt)
{
    JSContext *ctx = JS_NewContext(rt);
    if (!ctx)
        return NULL;
    if (!js_init_module_std(ctx, "std") || !js_init_module_os(ctx, "os") ||
        !js_init_module_bjson(ctx, "bjson")) {
        JS_FreeContext(ctx);
        return NULL;
    }
    return ctx;
}

/* the JS environment of the Python program (QPHost): the std, os and
   bjson modules, the globals of js_std_add_helpers() (setTimeout()...)
   and the module loader of quickjs-libc; the Python threads run while
   the event loop waits */
static int qpy_host_init(JSContext *ctx, int argc, char **argv)
{
    JSRuntime *rt = JS_GetRuntime(ctx);

    js_std_set_worker_new_context_func(qpy_new_worker_context);
    js_std_init_handlers(rt);
    js_std_set_wait_hooks(rt, JS_PythonSaveThread, JS_PythonRestoreThread);
    JS_SetModuleLoaderFunc2(rt, js_module_normalize, js_module_loader,
                            js_module_check_attributes, NULL);
    if (!js_init_module_std(ctx, "std") || !js_init_module_os(ctx, "os") ||
        !js_init_module_bjson(ctx, "bjson"))
        return -1;
    js_std_add_helpers(ctx, argc, argv);
    return JS_HasException(ctx) ? -1 : 0;
}

static const QPHost qpy_host = {
    qpy_host_init,
    js_std_poll,
    js_std_free_handlers,
};

static int dump_ast_json_batch(void)
{
    JSRuntime *rt;
    JSContext *ctx;
    int ret;

    rt = JS_NewRuntime();
    if (!rt) {
        fprintf(stderr, "qpy: cannot allocate JS runtime\n");
        return 1;
    }
    ctx = JS_NewContext(rt);
    if (!ctx) {
        fprintf(stderr, "qpy: cannot allocate JS context\n");
        JS_FreeRuntime(rt);
        return 1;
    }
    ret = qp_dump_ast_json_batch(ctx, stdin, stdout);
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
    return ret < 0 ? 1 : 0;
}

int main(int argc, char **argv)
{
    const uint8_t *base, *frozen;
    size_t base_size, frozen_size;

#ifdef CONFIG_PYTHON_CAPI
    /* The CPython modules that link a system library (libqpcapi.a,
       capi/Setup.qp) become built-in modules; must precede the first
       Python context.  The self-contained ones are registered by the
       runtime. */
    if (PyImport_ExtendInittab(_PyQp_ExtInittab) < 0) {
        fprintf(stderr, "qpy: cannot register the C extension modules\n");
        return 1;
    }
#endif
    if (argc == 2 && strcmp(argv[1], "--dump-ast-json-batch") == 0)
        return dump_ast_json_batch();
    if (argc == 4 && strcmp(argv[1], "--write-frozen-modules") == 0)
        return qp_write_frozen_modules(argv[2], argv[3]);
    if ((argc == 4 || argc == 5) && strcmp(argv[1], "--write-base-snapshot") == 0) {
        if (argc == 5) {
            frozen = read_whole_file(argv[4], &frozen_size);
            if (!frozen)
                return 1;
            qp_set_frozen_modules(frozen, frozen_size);
        }
        return qp_write_base_snapshot(argv[2], argv[3]);
    }
    frozen = qpy_frozen_modules(&frozen_size);
    if (frozen)
        qp_set_frozen_modules(frozen, frozen_size);
    base = qpy_base_snapshot(&base_size);
    if (base)
        qp_set_base_snapshot(base, base_size);
    qp_set_host(&qpy_host);
    return qp_main(argc, argv);
}
