/*
 * qpy: Python front end of dquickts.
 *
 * Currently exposes the Python 3.15 parser only:
 *   qpy --dump-ast-json-batch   serve the JSON-lines AST protocol on
 *                               stdin/stdout (see python/quickpy.h)
 * Running Python code is not available yet; any other invocation reports
 * that and exits with status 2.
 */
#include <stdio.h>
#include <string.h>

#include "quickjs.h"
#include "python/quickpy.h"

int main(int argc, char **argv)
{
    JSRuntime *rt;
    JSContext *ctx;
    int ret;

    if (argc != 2 || strcmp(argv[1], "--dump-ast-json-batch") != 0) {
        fprintf(stderr, "qpy: running Python code is not implemented yet\n");
        return 2;
    }
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
