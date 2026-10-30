/*
 * Mid-execution hibernation test host.
 *
 * Every script is run twice. The reference run executes it normally. The
 * hibernating run requests a suspension from the interrupt handler every
 * N calls; whenever the runtime is suspended (or between two jobs), it is
 * written to a snapshot, freed, rebuilt from the snapshot in a new
 * runtime and resumed. Both runs must print the same output.
 *
 *   test_hibernate [-v] [-n period] script.js...
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "quickjs.h"

typedef struct {
    char *buf;
    size_t len, size;
} Output;

static Output out;
static int verbose;
static int period = 1;

static void out_append(const char *s, size_t len)
{
    if (out.len + len + 1 > out.size) {
        out.size = (out.len + len + 1) * 2;
        out.buf = realloc(out.buf, out.size);
    }
    memcpy(out.buf + out.len, s, len);
    out.len += len;
    out.buf[out.len] = '\0';
}

static JSValue js_print(JSContext *ctx, JSValueConst this_val,
                        int argc, JSValueConst *argv)
{
    int i;
    for(i = 0; i < argc; i++) {
        size_t len;
        const char *str = JS_ToCStringLen(ctx, &len, argv[i]);
        if (!str)
            return JS_EXCEPTION;
        if (i != 0)
            out_append(" ", 1);
        out_append(str, len);
        JS_FreeCString(ctx, str);
    }
    out_append("\n", 1);
    return JS_UNDEFINED;
}

static const void * const host_natives[] = {
    (const void *)js_print,
};

static const JSSnapshotHost host = {
    .natives = host_natives,
    .native_count = sizeof(host_natives) / sizeof(host_natives[0]),
};

typedef struct {
    int calls; /* interrupt handler calls */
    int hibernations;
    size_t max_size;
    uint64_t branches;
} Stats;

static int interrupt_handler(JSRuntime *rt, void *opaque)
{
    Stats *st = opaque;
    if (++st->calls % period == 0)
        JS_RequestSuspend(rt);
    return 0;
}

static void print_exception(JSContext *ctx)
{
    JSValue exc = JS_GetException(ctx);
    const char *str = JS_ToCString(ctx, exc);
    char line[1024];
    snprintf(line, sizeof(line), "exception: %s\n", str ? str : "?");
    out_append(line, strlen(line));
    if (str)
        JS_FreeCString(ctx, str);
    JS_FreeValue(ctx, exc);
}

static char *load_file(const char *filename, size_t *plen)
{
    FILE *f = fopen(filename, "rb");
    char *buf;
    long len;
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    len = ftell(f);
    fseek(f, 0, SEEK_SET);
    buf = malloc(len + 1);
    if (fread(buf, 1, len, f) != (size_t)len) {
        fclose(f);
        free(buf);
        return NULL;
    }
    buf[len] = '\0';
    fclose(f);
    *plen = len;
    return buf;
}

static JSRuntime *new_runtime(Stats *st)
{
    JSRuntime *rt = JS_NewRuntime();
    if (st)
        JS_SetInterruptHandler(rt, interrupt_handler, st);
    return rt;
}

static JSContext *new_context(JSRuntime *rt)
{
    JSContext *ctx = JS_NewContext(rt);
    JSValue global = JS_GetGlobalObject(ctx);
    JS_SetPropertyStr(ctx, global, "print", JS_NewCFunction(ctx, js_print, "print", 1));
    JS_FreeValue(ctx, global);
    return ctx;
}

/* print the completion value of the script or its exception */
static void report_completion(JSContext *ctx, JSValue val)
{
    if (JS_IsException(val)) {
        print_exception(ctx);
    } else {
        const char *str = JS_ToCString(ctx, val);
        char line[1024];
        snprintf(line, sizeof(line), "completion: %s\n", str ? str : "?");
        out_append(line, strlen(line));
        if (str)
            JS_FreeCString(ctx, str);
        JS_FreeValue(ctx, val);
    }
}

static char *run_plain(const char *src, size_t len, const char *filename,
                       uint64_t *pbranches)
{
    JSRuntime *rt = new_runtime(NULL);
    JSContext *ctx = new_context(rt), *ctx1;
    int ret;

    out.len = 0;
    out_append("", 0);
    report_completion(ctx, JS_Eval(ctx, src, len, filename, JS_EVAL_TYPE_GLOBAL));
    while ((ret = JS_ExecutePendingJob(rt, &ctx1)) != 0) {
        if (ret < 0)
            print_exception(ctx1);
    }
    *pbranches = JS_GetProgramBranchCount(ctx);
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
    return strdup(out.buf);
}

/* -d: every hibernation writes a delta of the base written once the
   natives are set up, before the script runs */
static int delta_mode;
static uint8_t *base_buf;
static size_t base_size;

/* write a snapshot and copy it out of the runtime */
static uint8_t *write_snapshot(JSContext *ctx, size_t *psize, int flags)
{
    uint8_t *snap = JS_WriteSnapshot(ctx, psize, &host, NULL, 0, flags), *buf;
    if (!snap) {
        print_exception(ctx);
        return NULL;
    }
    buf = malloc(*psize);
    memcpy(buf, snap, *psize);
    js_free(ctx, snap);
    return buf;
}

/* snapshot the runtime, free it and rebuild it in a new runtime */
static JSRuntime *hibernate(JSRuntime *rt, JSContext **pctx, Stats *st)
{
    JSContext *ctx = *pctx;
    JSSnapshotResult res;
    char errbuf[256];
    uint8_t *buf;
    size_t size;
    int ret;

    buf = write_snapshot(ctx, &size, base_buf ? JS_SNAPSHOT_DELTA : 0);
    if (!buf)
        return NULL;
    st->branches += JS_GetProgramBranchCount(ctx);
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);

    st->hibernations++;
    if (size > st->max_size)
        st->max_size = size;
    rt = new_runtime(st);
    if (base_buf)
        ret = JS_ReadSnapshot(rt, base_buf, base_size, buf, size, &host, &res,
                              errbuf, sizeof(errbuf));
    else
        ret = JS_ReadSnapshot(rt, buf, size, NULL, 0, &host, &res,
                              errbuf, sizeof(errbuf));
    if (ret) {
        char line[512];
        snprintf(line, sizeof(line), "snapshot read error: %s\n", errbuf);
        out_append(line, strlen(line));
        JS_FreeRuntime(rt);
        free(buf);
        return NULL;
    }
    free(buf);
    *pctx = res.contexts[0];
    JS_FreeSnapshotResult(rt, &res);
    return rt;
}

/* run 'val = f()' to completion, hibernating whenever it is suspended */
static JSRuntime *complete(JSRuntime *rt, JSContext **pctx, JSValue *pval, Stats *st)
{
    while (JS_IsException(*pval) && JS_IsSuspended(rt)) {
        rt = hibernate(rt, pctx, st);
        if (!rt)
            return NULL;
        *pval = JS_Resume(*pctx);
    }
    return rt;
}

static char *run_hibernating(const char *src, size_t len, const char *filename, Stats *st)
{
    JSRuntime *rt = new_runtime(st);
    JSContext *ctx = new_context(rt);
    JSValue val;

    out.len = 0;
    out_append("", 0);
    memset(st, 0, sizeof(*st));
    if (delta_mode) {
        base_buf = write_snapshot(ctx, &base_size, JS_SNAPSHOT_BASE);
        if (!base_buf)
            return strdup(out.buf);
    }
    val = JS_Eval(ctx, src, len, filename, JS_EVAL_TYPE_GLOBAL);
    rt = complete(rt, &ctx, &val, st);
    if (!rt)
        return strdup(out.buf);
    report_completion(ctx, val);
    for(;;) {
        JSContext *ctx1;
        int ret;
        /* hibernate between jobs too */
        rt = hibernate(rt, &ctx, st);
        if (!rt)
            return strdup(out.buf);
        ret = JS_ExecutePendingJob(rt, &ctx1);
        if (ret == 0)
            break;
        if (ret < 0)
            print_exception(ctx1);
    }
    st->branches += JS_GetProgramBranchCount(ctx);
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
    return strdup(out.buf);
}

int main(int argc, char **argv)
{
    int i, failed = 0;

    for(i = 1; i < argc && argv[i][0] == '-'; i++) {
        if (!strcmp(argv[i], "-v"))
            verbose = 1;
        else if (!strcmp(argv[i], "-n") && i + 1 < argc)
            period = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-d"))
            delta_mode = 1;
    }
    for(; i < argc; i++) {
        Stats st;
        uint64_t plain_branches = 0;
        size_t len;
        char *src = load_file(argv[i], &len);
        char *expected, *got;
        if (!src) {
            fprintf(stderr, "%s: cannot read\n", argv[i]);
            failed++;
            continue;
        }
        expected = run_plain(src, len, argv[i], &plain_branches);
        got = run_hibernating(src, len, argv[i], &st);
        if (strcmp(expected, got) != 0) {
            printf("FAIL %s\n--- expected:\n%s--- got:\n%s", argv[i], expected, got);
            failed++;
        } else if (plain_branches != st.branches) {
            printf("FAIL %s: branch count %llu != %llu\n", argv[i],
                   (unsigned long long)st.branches,
                   (unsigned long long)plain_branches);
            failed++;
        } else {
            if (delta_mode)
                printf("ok   %s (%d interrupts, %d hibernations, base %zu bytes, max delta %zu bytes)\n",
                       argv[i], st.calls, st.hibernations, base_size, st.max_size);
            else
                printf("ok   %s (%d interrupts, %d hibernations, max %zu bytes)\n",
                       argv[i], st.calls, st.hibernations, st.max_size);
            if (verbose)
                printf("%s", got);
        }
        free(expected);
        free(got);
        free(src);
        free(base_buf);
        base_buf = NULL;
    }
    free(out.buf);
    return failed != 0;
}
