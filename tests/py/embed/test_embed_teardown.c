/*
 * Python embedder teardown tests: an embedder that uses Python through the
 * public API (JS_InitPython, JS_PythonImport, JS_EvalPython) and then calls
 * JS_FreeContext / JS_FreeRuntime must leave nothing behind (JS_FreeRuntime
 * asserts that every GC object was freed; the ASAN build reports the leaks).
 *
 * Usage: test_embed_teardown HOME [SCENARIO...]
 *   HOME is the Python home (the directory that contains pylib).  Without
 *   scenarios, every scenario runs in its own child process (an assertion
 *   failure aborts the child) and then all of them run in sequence in this
 *   process (fresh runtimes after a teardown).
 */
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <pthread.h>

#include "../../../quickjs.h"

static const char *home;

#define CHECK(cond) check((cond), #cond, __LINE__)

static void check(int cond, const char *msg, int line)
{
    if (!cond) {
        fprintf(stderr, "test_embed_teardown:%d: check failed: %s\n", line, msg);
        exit(1);
    }
}

static JSRuntime *new_runtime(void)
{
    JSRuntime *rt = JS_NewRuntime();
    CHECK(rt != NULL);
    return rt;
}

static JSContext *new_python_context(JSRuntime *rt)
{
    JSPythonConfig cfg;
    JSContext *ctx = JS_NewContext(rt);
    CHECK(ctx != NULL);
    memset(&cfg, 0, sizeof(cfg));
    cfg.home = home;
    cfg.hash_seed = "0";
    CHECK(JS_InitPython(ctx, &cfg) == 0);
    return ctx;
}

static void print_exception(JSContext *ctx)
{
    JSValue exc = JS_GetException(ctx);
    const char *s = JS_ToCString(ctx, exc);
    fprintf(stderr, "exception: %s\n", s ? s : "?");
    JS_FreeCString(ctx, s);
    JS_FreeValue(ctx, exc);
}

static JSValue py(JSContext *ctx, const char *src, int mode)
{
    JSValue v = JS_EvalPython(ctx, src, strlen(src), "<embed>", mode);
    if (JS_IsException(v)) {
        print_exception(ctx);
        CHECK(!"JS_EvalPython failed");
    }
    return v;
}

static void py_exec(JSContext *ctx, const char *src)
{
    JS_FreeValue(ctx, py(ctx, src, JS_PY_EVAL_EXEC));
}

/* str(eval(src)) == expected */
static void py_expect(JSContext *ctx, const char *src, const char *expected)
{
    JSValue v = py(ctx, src, JS_PY_EVAL_EVAL);
    const char *s = JS_ToCString(ctx, v);
    CHECK(s != NULL);
    if (strcmp(s, expected) != 0) {
        fprintf(stderr, "%s: got '%s', expected '%s'\n", src, s, expected);
        CHECK(!"unexpected value");
    }
    JS_FreeCString(ctx, s);
    JS_FreeValue(ctx, v);
}

static JSValue import(JSContext *ctx, const char *name)
{
    JSValue m = JS_PythonImport(ctx, name);
    if (JS_IsException(m)) {
        print_exception(ctx);
        CHECK(!"JS_PythonImport failed");
    }
    return m;
}

/* the rquickjs import_json test: json.dumps / json.loads called from C */
static void scenario_json(void)
{
    JSRuntime *rt = new_runtime();
    JSContext *ctx = new_python_context(rt);
    JSValue json = import(ctx, "json");
    JSValue dumps = JS_GetPropertyStr(ctx, json, "dumps");
    JSValue loads = JS_GetPropertyStr(ctx, json, "loads");
    JSValue arr = JS_NewArray(ctx), res, obj;
    const char *s;
    int i;

    for (i = 0; i < 3; i++)
        JS_SetPropertyUint32(ctx, arr, i, JS_NewInt32(ctx, i + 1));
    res = JS_Call(ctx, dumps, JS_UNDEFINED, 1, (JSValueConst *)&arr);
    CHECK(!JS_IsException(res));
    s = JS_ToCString(ctx, res);
    CHECK(s && !strcmp(s, "[1, 2, 3]"));
    JS_FreeCString(ctx, s);
    JS_FreeValue(ctx, res);

    res = JS_NewString(ctx, "{\"a\": [1, 2], \"b\": \"x\"}");
    obj = JS_Call(ctx, loads, JS_UNDEFINED, 1, (JSValueConst *)&res);
    JS_FreeValue(ctx, res);
    CHECK(!JS_IsException(obj));
    res = JS_GetPropertyStr(ctx, obj, "b");
    s = JS_ToCString(ctx, res);
    CHECK(s && !strcmp(s, "x"));
    JS_FreeCString(ctx, s);
    JS_FreeValue(ctx, res);

    /* a missing module: ModuleNotFoundError, caught */
    res = JS_PythonImport(ctx, "no_such_module_here");
    CHECK(JS_IsException(res));
    res = JS_GetException(ctx);
    {
        JSValue name = py(ctx, "lambda e: type(e).__name__", JS_PY_EVAL_EVAL);
        JSValue n = JS_Call(ctx, name, JS_UNDEFINED, 1, (JSValueConst *)&res);
        s = JS_ToCString(ctx, n);
        CHECK(s && !strcmp(s, "ModuleNotFoundError"));
        JS_FreeCString(ctx, s);
        JS_FreeValue(ctx, n);
        JS_FreeValue(ctx, name);
    }
    JS_FreeValue(ctx, res);

    JS_FreeValue(ctx, obj);
    JS_FreeValue(ctx, arr);
    JS_FreeValue(ctx, loads);
    JS_FreeValue(ctx, dumps);
    JS_FreeValue(ctx, json);
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
}

/* modules built on the CPython C modules of the engine */
static void scenario_modules(void)
{
    JSRuntime *rt = new_runtime();
    JSContext *ctx = new_python_context(rt);
    static const char *const names[] = {
        "re", "itertools", "decimal", "datetime", "hashlib", "math",
    };
    size_t i;

    for (i = 0; i < sizeof(names) / sizeof(names[0]); i++)
        JS_FreeValue(ctx, import(ctx, names[i]));
    py_exec(ctx,
            "import re, itertools, decimal, datetime, hashlib, math\n"
            "R = ' '.join(map(str, [\n"
            "    re.sub(r'\\d+', '#', 'a1b22'),\n"
            "    list(itertools.accumulate([1, 2, 3])),\n"
            "    decimal.Decimal(1) / 8,\n"
            "    datetime.date(2024, 2, 28) + datetime.timedelta(days=2),\n"
            "    hashlib.sha256(b'x').hexdigest()[:8],\n"
            "    math.comb(5, 2)]))\n"
            "pat = re.compile('(a)(b)?')\n"
            "it = itertools.count()\n"
            "ctx = decimal.localcontext()\n"
            "now = datetime.datetime(2024, 1, 1, tzinfo=datetime.timezone.utc)\n");
    py_expect(ctx, "R", "a#b# [1, 3, 6] 0.125 2024-03-01 2d711642 10");
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
}

/* a Python exception left pending in the context, and one held by a
   Python global (exception -> traceback -> frames) */
static void scenario_pending_exception(void)
{
    JSRuntime *rt = new_runtime();
    JSContext *ctx = new_python_context(rt);
    JSValue v;

    JS_FreeValue(ctx, import(ctx, "json"));
    py_exec(ctx,
            "import json\n"
            "try:\n"
            "    json.loads('{')\n"
            "except ValueError as e:\n"
            "    saved = e\n");
    v = JS_EvalPython(ctx, "json.loads('[1,')", 17, "<embed>", JS_PY_EVAL_EVAL);
    CHECK(JS_IsException(v));
    /* not retrieved: JS_FreeRuntime releases it */
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
}

/* a failed import left pending (a Rust panic unwinding through the
   embedder): the traceback holds the import machinery's module locks,
   whose weakref callbacks are queued when it dies at teardown */
static void scenario_pending_import_error(void)
{
    JSRuntime *rt = new_runtime();
    JSContext *ctx = new_python_context(rt);
    JSValue v = JS_PythonImport(ctx, "no_such_module_here");
    CHECK(JS_IsException(v));
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
}

static char finalizer_log[256];

static JSValue js_record(JSContext *ctx, JSValueConst this_val,
                         int argc, JSValueConst *argv)
{
    const char *s = JS_ToCString(ctx, argv[0]);
    if (!s)
        return JS_EXCEPTION;
    if (strlen(finalizer_log) + strlen(s) + 2 < sizeof(finalizer_log)) {
        strcat(finalizer_log, s);
        strcat(finalizer_log, ";");
    }
    JS_FreeCString(ctx, s);
    return JS_UNDEFINED;
}

/* an atexit handler and the __del__ of a cycle run at the teardown, as at
   the exit of qpy and CPython */
static void scenario_finalizers(void)
{
    JSRuntime *rt = new_runtime();
    JSContext *ctx = new_python_context(rt);
    JSValue record, setter;

    finalizer_log[0] = '\0';
    record = JS_NewCFunction(ctx, js_record, "record", 1);
    setter = py(ctx, "lambda f: globals().__setitem__('record', f)", JS_PY_EVAL_EVAL);
    JS_FreeValue(ctx, JS_Call(ctx, setter, JS_UNDEFINED, 1, (JSValueConst *)&record));
    JS_FreeValue(ctx, setter);
    JS_FreeValue(ctx, record);
    py_exec(ctx,
            "import atexit, sys\n"
            "class D:\n"
            "    def __del__(self, rec=record):\n"
            "        rec('del')\n"
            "d = D()\n"
            "d.cycle = d\n"
            "atexit.register(lambda rec=record: rec('atexit %s' % sys.is_finalizing()))\n");
    JS_FreeContext(ctx);
    CHECK(finalizer_log[0] == '\0');
    JS_FreeRuntime(rt);
    if (strcmp(finalizer_log, "atexit False;del;") != 0) {
        fprintf(stderr, "finalizers: %s\n", finalizer_log);
        CHECK(!"the finalizers did not run at the teardown");
    }
}

/* reference cycles through JS objects: Python object -> JS object ->
   Python object, a JS closure holding a Python module, and a C-module
   object (re.Pattern, decimal.Decimal) on the cycle */
static void scenario_cycle(void)
{
    JSRuntime *rt = new_runtime();
    JSContext *ctx = new_python_context(rt);
    JSValue link, jsobj, args[2], res, glob, f;

    py_exec(ctx,
            "import re, decimal\n"
            "class Node:\n"
            "    def __init__(self):\n"
            "        self.pat = re.compile('x+')\n"
            "        self.num = decimal.Decimal('1.5')\n"
            "node = Node()\n"
            "def link(n, o):\n"
            "    n.js = o\n"
            "    return [n, o]\n");
    link = py(ctx, "link", JS_PY_EVAL_EVAL);
    jsobj = JS_NewObject(ctx);
    args[0] = py(ctx, "node", JS_PY_EVAL_EVAL);
    /* jsobj.node = node: node.js = jsobj closes the cycle */
    JS_SetPropertyStr(ctx, jsobj, "node", JS_DupValue(ctx, args[0]));
    args[1] = jsobj;
    res = JS_Call(ctx, link, JS_UNDEFINED, 2, (JSValueConst *)args);
    CHECK(!JS_IsException(res));
    /* the returned list (an Array) joins the cycle too */
    JS_SetPropertyStr(ctx, jsobj, "pair", res);
    JS_FreeValue(ctx, args[0]);
    JS_FreeValue(ctx, link);

    /* a JS closure that captures the Python json module, stored on both sides */
    glob = JS_GetGlobalObject(ctx);
    JS_SetPropertyStr(ctx, glob, "json", import(ctx, "json"));
    JS_SetPropertyStr(ctx, glob, "holder", JS_DupValue(ctx, jsobj));
    f = JS_Eval(ctx,
                "(() => { const j = json; holder.f = (x) => j.dumps(x);"
                " return holder.f; })()",
                strlen("(() => { const j = json; holder.f = (x) => j.dumps(x);"
                       " return holder.f; })()"),
                "<embed>", JS_EVAL_TYPE_GLOBAL);
    CHECK(!JS_IsException(f));
    {
        JSValue setf = py(ctx, "lambda f: globals().__setitem__('jsf', f)", JS_PY_EVAL_EVAL);
        JS_FreeValue(ctx, JS_Call(ctx, setf, JS_UNDEFINED, 1, (JSValueConst *)&f));
        JS_FreeValue(ctx, setf);
    }
    py_expect(ctx, "jsf([1, 'a'])", "[1, \"a\"]");
    JS_FreeValue(ctx, f);
    JS_FreeValue(ctx, glob);
    JS_FreeValue(ctx, jsobj);
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
}

/* a top-level-await evaluation whose Task is still pending */
static void scenario_pending_task(void)
{
    JSRuntime *rt = new_runtime();
    JSContext *ctx = new_python_context(rt);
    const char *src = "import asyncio, json\nawait asyncio.sleep(0)\nx = json.dumps(1)\n";
    JSValue p = JS_EvalPython(ctx, src, strlen(src), "<embed>",
                              JS_PY_EVAL_EXEC | JS_PY_EVAL_FLAG_ASYNC);
    CHECK(!JS_IsException(p));
    JS_FreeValue(ctx, p);
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
}

/* two Python contexts in one runtime: each has its own interpreter; the
   CPython C modules are bound to the first one that imports one */
static void scenario_two_contexts(void)
{
    JSRuntime *rt = new_runtime();
    JSContext *ctx1 = new_python_context(rt);
    JSContext *ctx2 = new_python_context(rt);
    JSValue v;

    JS_FreeValue(ctx1, import(ctx1, "json"));
    py_exec(ctx1, "import re\nR = re.sub('a', 'b', 'aa')\n");
    py_expect(ctx1, "R", "bb");
    py_exec(ctx2, "import keyword\nR = [k for k in sorted({'b': 1, 'a': 2})]\n");
    py_expect(ctx2, "R", "a,b");
    py_expect(ctx2, "keyword.iskeyword('def')", "true");
    /* the second interpreter cannot import a C module */
    v = JS_PythonImport(ctx2, "_json");
    CHECK(JS_IsException(v));
    {
        JSValue exc = JS_GetException(ctx2);
        JSValue name = py(ctx2, "lambda e: type(e).__name__", JS_PY_EVAL_EVAL);
        JSValue n = JS_Call(ctx2, name, JS_UNDEFINED, 1, (JSValueConst *)&exc);
        const char *s = JS_ToCString(ctx2, n);
        CHECK(s && !strcmp(s, "ImportError"));
        JS_FreeCString(ctx2, s);
        JS_FreeValue(ctx2, n);
        JS_FreeValue(ctx2, name);
        JS_FreeValue(ctx2, exc);
    }
    /* a context freed first, the other last, both with Python state */
    JS_FreeContext(ctx2);
    py_expect(ctx1, "__import__('json').dumps([R])", "[\"bb\"]");
    JS_FreeContext(ctx1);
    JS_FreeRuntime(rt);
}

/* the C-module context freed while another Python context lives on */
static void scenario_free_capi_context_first(void)
{
    JSRuntime *rt = new_runtime();
    JSContext *ctx1 = new_python_context(rt);
    JSContext *ctx2 = new_python_context(rt);

    JS_FreeValue(ctx1, import(ctx1, "decimal"));
    JS_FreeContext(ctx1);
    py_expect(ctx2, "sum(range(5))", "10");
    JS_FreeContext(ctx2);
    JS_FreeRuntime(rt);
}

/* Python initialized implicitly by the first JS_EvalPython (no
   JS_InitPython), and a context that never initializes Python */
static void scenario_implicit_init(void)
{
    JSRuntime *rt = new_runtime();
    JSContext *ctx = JS_NewContext(rt);
    JSContext *plain = JS_NewContext(rt);
    CHECK(ctx && plain);
    /* the default configuration finds the stdlib through QPYHOME */
    setenv("QPYHOME", home, 1);
    py_expect(ctx, "__import__('itertools').__name__", "itertools");
    JS_FreeContext(plain);
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
}

/* ---- the rquickjs tests (rquickjs/core/src/context/python.rs), in the
   order `cargo test -- --test-threads=1` runs them: one runtime each,
   created and freed in one process ---- */

static void run_jobs(JSRuntime *rt)
{
    JSContext *jctx;
    while (JS_ExecutePendingJob(rt, &jctx) > 0)
        continue;
}

/* the type name of the exception pending in ctx (retrieved) */
static void expect_exception(JSContext *ctx, const char *type_name)
{
    JSValue exc = JS_GetException(ctx);
    JSValue f = py(ctx, "lambda e: type(e).__name__", JS_PY_EVAL_EVAL);
    JSValue n = JS_Call(ctx, f, JS_UNDEFINED, 1, (JSValueConst *)&exc);
    const char *s = JS_ToCString(ctx, n);
    if (!s || strcmp(s, type_name) != 0) {
        fprintf(stderr, "exception %s, expected %s\n", s ? s : "?", type_name);
        CHECK(!"unexpected exception type");
    }
    JS_FreeCString(ctx, s);
    JS_FreeValue(ctx, n);
    JS_FreeValue(ctx, f);
    JS_FreeValue(ctx, exc);
}

static void scenario_rq_builtin_c_modules(void)
{
    JSRuntime *rt = new_runtime();
    JSContext *ctx = new_python_context(rt);
    py_exec(ctx,
            "import sys, re, itertools, decimal, hashlib, math\n"
            "C_MODULES = ' '.join(map(str, [\n"
            "    '_sre' in sys.builtin_module_names,\n"
            "    re.sub(r'\\d+', '#', 'a1b22'),\n"
            "    list(itertools.accumulate([1, 2, 3])),\n"
            "    decimal.Decimal(1) / 8,\n"
            "    hashlib.sha256(b'x').hexdigest()[:8],\n"
            "    math.comb(5, 2)]))\n");
    py_expect(ctx, "C_MODULES", "True a#b# [1, 3, 6] 0.125 2d711642 10");
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
}

static void scenario_rq_eval_values(void)
{
    JSRuntime *rt = new_runtime();
    JSContext *ctx = new_python_context(rt);
    JSValue dict, f, r;
    const char *src = "(o) => o.a + o.b.length";
    int32_t n;

    py_expect(ctx, "6 * 7", "42");
    py_expect(ctx, "'py' + 'thon'", "python");
    py_expect(ctx, "2 ** 40", "1099511627776");
    py_expect(ctx, "[x * x for x in range(4)]", "0,1,4,9");
    dict = py(ctx, "{'a': 1, 'b': [1, 2], 'c': {'d': 'e'}}", JS_PY_EVAL_EVAL);
    f = JS_Eval(ctx, src, strlen(src), "<embed>", JS_EVAL_TYPE_GLOBAL);
    CHECK(!JS_IsException(f));
    r = JS_Call(ctx, f, JS_UNDEFINED, 1, (JSValueConst *)&dict);
    CHECK(!JS_ToInt32(ctx, &n, r) && n == 3);
    JS_FreeValue(ctx, r);
    JS_FreeValue(ctx, f);
    JS_FreeValue(ctx, dict);
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
}

static void scenario_rq_exceptions(void)
{
    JSRuntime *rt = new_runtime();
    JSContext *ctx = new_python_context(rt);
    JSValue v, fail, arg;

    v = JS_EvalPython(ctx, "1 / 0", 5, NULL, JS_PY_EVAL_EVAL);
    CHECK(JS_IsException(v));
    expect_exception(ctx, "ZeroDivisionError");
    py_exec(ctx, "def fail(msg):\n    raise ValueError(msg)\n");
    fail = py(ctx, "fail", JS_PY_EVAL_EVAL);
    arg = JS_NewString(ctx, "bad");
    v = JS_Call(ctx, fail, JS_UNDEFINED, 1, (JSValueConst *)&arg);
    CHECK(JS_IsException(v));
    expect_exception(ctx, "ValueError");
    v = JS_EvalPython(ctx, "1 +", 3, "bad.py", JS_PY_EVAL_EVAL);
    CHECK(JS_IsException(v));
    expect_exception(ctx, "SyntaxError");
    CHECK(!JS_HasException(ctx));
    py_expect(ctx, "1 + 1", "2");
    JS_FreeValue(ctx, arg);
    JS_FreeValue(ctx, fail);
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
}

static JSValue js_triple(JSContext *ctx, JSValueConst this_val,
                         int argc, JSValueConst *argv)
{
    int32_t x;
    if (JS_ToInt32(ctx, &x, argv[0]))
        return JS_EXCEPTION;
    return JS_NewInt32(ctx, x * 3);
}

static void scenario_rq_exec_and_call_functions(void)
{
    JSRuntime *rt = new_runtime();
    JSContext *ctx = new_python_context(rt);
    JSValue add, apply, args[2], r, main_mod;
    int32_t n;

    py_exec(ctx, "def add(a, b=10):\n    return a + b\ncounter = 0\n");
    add = py(ctx, "add", JS_PY_EVAL_EVAL);
    args[0] = JS_NewInt32(ctx, 1);
    args[1] = JS_NewInt32(ctx, 2);
    r = JS_Call(ctx, add, JS_UNDEFINED, 2, (JSValueConst *)args);
    CHECK(!JS_ToInt32(ctx, &n, r) && n == 3);
    JS_FreeValue(ctx, r);
    /* Rust (C) functions called by Python */
    apply = py(ctx, "lambda f, x: f(x)", JS_PY_EVAL_EVAL);
    args[0] = JS_NewCFunction(ctx, js_triple, "triple", 1);
    args[1] = JS_NewInt32(ctx, 14);
    r = JS_Call(ctx, apply, JS_UNDEFINED, 2, (JSValueConst *)args);
    CHECK(!JS_ToInt32(ctx, &n, r) && n == 42);
    JS_FreeValue(ctx, r);
    JS_FreeValue(ctx, args[0]);
    py_exec(ctx, "counter += 5");
    main_mod = import(ctx, "__main__");
    r = JS_GetPropertyStr(ctx, main_mod, "counter");
    CHECK(!JS_ToInt32(ctx, &n, r) && n == 5);
    JS_FreeValue(ctx, r);
    JS_FreeValue(ctx, main_mod);
    JS_FreeValue(ctx, apply);
    JS_FreeValue(ctx, add);
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
}

/* init_config: two runtimes with the same hash seed hash alike */
static void hash_with_seed(const char *seed, char *out, size_t size)
{
    static char *argv[] = { (char *)"prog", (char *)"-x", NULL };
    JSRuntime *rt = new_runtime();
    JSContext *ctx = JS_NewContext(rt);
    JSPythonConfig cfg;
    JSValue v;
    const char *s;

    CHECK(ctx != NULL);
    memset(&cfg, 0, sizeof(cfg));
    cfg.argc = 2;
    cfg.argv = argv;
    cfg.home = home;
    cfg.hash_seed = seed;
    cfg.recursion_limit = 500;
    CHECK(JS_InitPython(ctx, &cfg) == 0);
    py_expect(ctx, "__import__('sys').argv", "prog,-x");
    py_expect(ctx, "__import__('sys').getrecursionlimit()", "500");
    v = py(ctx, "str(hash('rquickjs'))", JS_PY_EVAL_EVAL);
    s = JS_ToCString(ctx, v);
    CHECK(s != NULL);
    snprintf(out, size, "%s", s);
    JS_FreeCString(ctx, s);
    JS_FreeValue(ctx, v);
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
}

static void scenario_rq_init_config(void)
{
    char a[32], b[32], c[32];
    hash_with_seed("42", a, sizeof(a));
    hash_with_seed("42", b, sizeof(b));
    hash_with_seed("43", c, sizeof(c));
    CHECK(!strcmp(a, b) && strcmp(a, c) != 0);
}

static void scenario_rq_top_level_await(void)
{
    JSRuntime *rt = new_runtime();
    JSContext *ctx = new_python_context(rt);
    JSValue p, r;
    int32_t n;

    py_exec(ctx, "async def double(x):\n    return x * 2\n");
    p = py(ctx, "await double(21)", JS_PY_EVAL_EVAL | JS_PY_EVAL_FLAG_ASYNC);
    run_jobs(rt);
    CHECK(JS_PromiseState(ctx, p) == JS_PROMISE_FULFILLED);
    r = JS_PromiseResult(ctx, p);
    CHECK(!JS_ToInt32(ctx, &n, r) && n == 42);
    JS_FreeValue(ctx, r);
    JS_FreeValue(ctx, p);
    /* a rejection carries the Python exception */
    p = py(ctx, "await double(None)", JS_PY_EVAL_EVAL | JS_PY_EVAL_FLAG_ASYNC);
    run_jobs(rt);
    CHECK(JS_PromiseState(ctx, p) == JS_PROMISE_REJECTED);
    JS_Throw(ctx, JS_PromiseResult(ctx, p));
    expect_exception(ctx, "TypeError");
    JS_FreeValue(ctx, p);
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
}

/* runtimes in parallel threads (cargo's default test runner): the C API
   binds to one runtime at a time, the others get ImportError for the C
   modules, never a crash; pure Python works everywhere */
#define NTHREADS 4
#define NROUNDS 3

static void *thread_main(void *arg)
{
    int *bound = arg, round;

    for (round = 0; round < NROUNDS; round++) {
        JSRuntime *rt = new_runtime();
        JSContext *ctx = new_python_context(rt);
        JSValue v;

        py_expect(ctx, "sum(x * x for x in range(10))", "285");
        v = JS_PythonImport(ctx, "json");
        if (JS_IsException(v)) {
            expect_exception(ctx, "ImportError");
        } else {
            py_expect(ctx, "__import__('json').dumps({'a': [1, 2]})", "{\"a\": [1, 2]}");
            (*bound)++;
            JS_FreeValue(ctx, v);
        }
        JS_FreeContext(ctx);
        JS_FreeRuntime(rt);
    }
    return NULL;
}

static void scenario_threads(void)
{
    pthread_t th[NTHREADS];
    int bound[NTHREADS] = { 0 }, i, total = 0;

    for (i = 0; i < NTHREADS; i++)
        CHECK(pthread_create(&th[i], NULL, thread_main, &bound[i]) == 0);
    for (i = 0; i < NTHREADS; i++) {
        CHECK(pthread_join(th[i], NULL) == 0);
        total += bound[i];
    }
    CHECK(total >= 1);
    /* the C API is free again */
    scenario_json();
}

typedef struct {
    const char *name;
    void (*fn)(void);
} Scenario;

static const Scenario scenarios[] = {
    { "rq_builtin_c_modules", scenario_rq_builtin_c_modules },
    { "rq_eval_values", scenario_rq_eval_values },
    { "rq_exceptions", scenario_rq_exceptions },
    { "rq_exec_and_call_functions", scenario_rq_exec_and_call_functions },
    { "rq_import_json", scenario_json },
    { "rq_init_config", scenario_rq_init_config },
    { "rq_top_level_await", scenario_rq_top_level_await },
    { "modules", scenario_modules },
    { "pending_exception", scenario_pending_exception },
    { "pending_import_error", scenario_pending_import_error },
    { "finalizers", scenario_finalizers },
    { "cycle", scenario_cycle },
    { "pending_task", scenario_pending_task },
    { "two_contexts", scenario_two_contexts },
    { "free_capi_context_first", scenario_free_capi_context_first },
    { "implicit_init", scenario_implicit_init },
    { "threads", scenario_threads },
};
#define NSCENARIOS (sizeof(scenarios) / sizeof(scenarios[0]))

static const Scenario *find_scenario(const char *name)
{
    size_t i;
    for (i = 0; i < NSCENARIOS; i++) {
        if (!strcmp(scenarios[i].name, name))
            return &scenarios[i];
    }
    return NULL;
}

int main(int argc, char **argv)
{
    size_t i;
    int fail = 0;

    if (argc < 2) {
        fprintf(stderr, "usage: %s HOME [SCENARIO...]\n", argv[0]);
        return 2;
    }
    home = argv[1];
    if (argc > 2) {
        for (i = 2; i < (size_t)argc; i++) {
            const Scenario *s = find_scenario(argv[i]);
            if (!s) {
                fprintf(stderr, "unknown scenario: %s\n", argv[i]);
                return 2;
            }
            s->fn();
            printf("PASS %s\n", s->name);
        }
        return 0;
    }
    /* isolated: one process per scenario */
    for (i = 0; i < NSCENARIOS; i++) {
        int status;
        pid_t pid;
        fflush(stdout);
        pid = fork();
        CHECK(pid >= 0);
        if (pid == 0) {
            scenarios[i].fn();
            _exit(0);
        }
        CHECK(waitpid(pid, &status, 0) == pid);
        if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
            printf("PASS %s\n", scenarios[i].name);
        } else {
            printf("FAIL %s (%s %d)\n", scenarios[i].name,
                   WIFSIGNALED(status) ? "signal" : "exit",
                   WIFSIGNALED(status) ? WTERMSIG(status) : WEXITSTATUS(status));
            fail = 1;
        }
    }
    if (fail)
        return 1;
    /* sequential: fresh runtimes in one process after each teardown */
    for (i = 0; i < NSCENARIOS; i++)
        scenarios[i].fn();
    printf("PASS sequential (%u scenarios in one process)\n", (unsigned)NSCENARIOS);
    return 0;
}
