/*
 * qpy: Python 3.15 front end of dquickts.
 *
 *   qpy [option] ... [-c cmd | -m mod | file | -] [arg] ...
 *
 * The CPython command line (Modules/main.c, Python/initconfig.c) is
 * implemented by qp_main() (python/pythonrun.c) so that `qjs file.py` and
 * `qjs --python` share it.  Two extra options are kept for the test
 * harness and the build:
 *
 *   qpy --dump-ast-json-batch   serve the JSON-lines AST protocol on
 *                               stdin/stdout (see python/quickpy.h)
 *   qpy --write-base-snapshot FILE DIR
 *                               write the base snapshot of the qpy of
 *                               directory DIR (qp_write_base_snapshot),
 *                               which qpy-base.c embeds
 */
#include <stdio.h>
#include <string.h>

#include "quickjs.h"
#include "python/quickpy.h"
#ifdef CONFIG_PYTHON_CAPI
#include "capi/qpcapi.h"
#endif

/* qpy-base.c: the embedded base snapshot (NULL in qpy-nobase) */
const uint8_t *qpy_base_snapshot(size_t *psize);

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
    const uint8_t *base;
    size_t base_size;

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
    if (argc == 4 && strcmp(argv[1], "--write-base-snapshot") == 0)
        return qp_write_base_snapshot(argv[2], argv[3]);
    base = qpy_base_snapshot(&base_size);
    if (base)
        qp_set_base_snapshot(base, base_size);
    return qp_main(argc, argv);
}
