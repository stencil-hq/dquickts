/*
 * qpy: Python 3.15 front end of dquickts.
 *
 *   qpy [option] ... [-c cmd | -m mod | file | -] [arg] ...
 *
 * The CPython command line (Modules/main.c, Python/initconfig.c) is
 * implemented by qp_main() (python/pythonrun.c) so that `qjs file.py` and
 * `qjs --python` share it.  One extra option is kept for the test harness:
 *
 *   qpy --dump-ast-json-batch   serve the JSON-lines AST protocol on
 *                               stdin/stdout (see python/quickpy.h)
 */
#include <stdio.h>
#include <string.h>

#include "quickjs.h"
#include "python/quickpy.h"

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
    if (argc == 2 && strcmp(argv[1], "--dump-ast-json-batch") == 0)
        return dump_ast_json_batch();
    return qp_main(argc, argv);
}
