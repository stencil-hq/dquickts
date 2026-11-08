/*
 * libFuzzer target for the Python front end: compiles the input as a bytes
 * source (coding cookie / BOM detection) with qp_check(), i.e. lexer,
 * parser, __future__ checks, symbol table and compiler SyntaxErrors.
 * Modelled on fuzz/fuzz_compile.c.
 */
#include "quickjs.h"
#include "python/quickpy.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    JSRuntime *rt;
    JSContext *ctx;
    char *src;

    rt = JS_NewRuntime();
    if (!rt)
        return 0;
    /* same limits as fuzz/fuzz_common.c: 64 MB heap, 64 KB stack */
    JS_SetMemoryLimit(rt, 0x4000000);
    JS_SetMaxStackSize(rt, 0x10000);
    ctx = JS_NewContext(rt);
    if (!ctx) {
        JS_FreeRuntime(rt);
        return 0;
    }

    /* the tokenizer expects a NUL-terminated buffer */
    src = malloc(size + 1);
    if (src) {
        memcpy(src, data, size);
        src[size] = '\0';
        qp_check(ctx, src, size, "<fuzz>", QP_INPUT_FILE, 0, NULL);
        free(src);
    }

    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
    return 0;
}
