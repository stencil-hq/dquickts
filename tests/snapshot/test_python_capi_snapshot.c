/*
 * Snapshots of C-extension state (plan D5, doc/python-plan.md §4): a
 * session that imports CPython C modules (re, json, itertools,
 * collections, datetime, decimal, array, functools, random, and through
 * asyncio, csv, threading, logging and subprocess _asyncio, _csv and
 * _thread) and holds their objects is written, then read in a new process
 * (the C API binds one interpreter per process, so every phase runs in its
 * own process, as `qjs --drepl FILE --python` does across restarts):
 *
 *   phase 1  build the session; a C object that cannot be pickled and a
 *            running thread other than the main thread make the write
 *            fail with a TypeError naming them; write a full snapshot
 *            (JS_SNAPSHOT_BASE), change the session, write a delta of it
 *   phase 2  in a thread of its own (a main thread with another ident than
 *            the writer's): read base + delta, check every object
 *            (PASS/FAIL lines), change the session again and write a
 *            delta of the restored base (C objects in the base cells)
 *   phase 3  read base + second delta, check again
 *
 * Needs the engine built with CONFIG_PYTHON_CAPI and libqpcapi.a (top
 * Makefile: make test-snapshot-capi).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <pthread.h>
#include "quickjs.h"
#include "capi/qpcapi.h"

/* python/snapshot.c embedding entry points */
int qp_embed_init(JSContext *ctx);
int qp_embed_exec(JSContext *ctx, const char *src, const char *filename);

static const char setup_src[] =
    "import re, json, itertools, collections, datetime, decimal, array, functools, random\n"
    "import _json\n"
    "P = re.compile(r'(\\w+)@(\\w+)\\.com', re.I)\n"
    "DQ = collections.deque([1, 2, 3], maxlen=5); DQ.append(4)\n"
    "CNT = itertools.count(10, 3); next(CNT); next(CNT)\n"
    "CNT1 = itertools.count(5); next(CNT1)\n"
    "DT = datetime.datetime(2024, 5, 17, 12, 30, 15, 123456)\n"
    "DTZ = datetime.datetime(2024, 1, 1, tzinfo=datetime.timezone.utc)\n"
    "TD = datetime.timedelta(days=3, seconds=7)\n"
    "DEC = decimal.Decimal('3.14159')\n"
    "ARR = array.array('i', [1, 2, 3, 5, 8])\n"
    "JS = json.dumps({'a': [1, 2, {'b': None}], 'c': 'x'}, sort_keys=True)\n"
    "DD = collections.defaultdict(list); DD['k'].append(1)\n"
    "OD = collections.OrderedDict(a=1, b=2)\n"
    "CTR = collections.Counter('abracadabra')\n"
    "CTYPE = collections.deque\n"
    "CFUNC = json.decoder.scanstring\n"
    "CHAIN = itertools.chain\n"
    "APPEND = DQ.append\n"
    "MATCH = re.Pattern.match\n"
    "PART = functools.partial(int, base=2)\n"
    "RND = random.Random(42); RND.random()\n"
    "RND_NEXT = random.Random(42); RND_NEXT.random(); RND_EXPECT = RND_NEXT.random()\n"
    "@functools.lru_cache(maxsize=8)\n"
    "def square(x): return x * x\n"
    "square(3)\n"
    "class MyDeque(collections.deque):\n"
    "    def total(self): return sum(self)\n"
    "MYDQ = MyDeque([5, 6])\n"
    "MYDQ.tag = 'mine'\n"
    "SEEN = {DEC: 'decimal key', P: 'pattern key'}\n"
    "import asyncio, csv, threading, logging, subprocess, io, _asyncio\n"
    "csv.register_dialect('pipes', delimiter='|', quoting=csv.QUOTE_ALL)\n"
    "DIALECT = csv.get_dialect('pipes')\n"
    "LOCK = threading.Lock()\n"
    "HELD = threading.Lock(); HELD.acquire()\n"
    "RL = threading.RLock(); RL.acquire(); RL.acquire()\n"
    "FREE_RL = threading.RLock()\n"
    "LOCAL = threading.local(); LOCAL.x = 5\n"
    "EV = threading.Event(); EV.set()\n"
    "COND = threading.Condition()\n"
    "LOG = logging.getLogger('snap')\n"
    "DONE = threading.Thread(target=lambda: None); DONE.start(); DONE.join()\n"
    "MAIN = threading.main_thread()\n"
    "WRITER_IDENT = threading.get_ident()\n"
    "async def twice(x):\n"
    "    await asyncio.sleep(0)\n"
    "    return 2 * x\n"
    "assert asyncio.run(twice(1)) == 2\n";

/* change between the full snapshot and the first delta */
static const char change1_src[] =
    "DQ.append(7)\n"
    "LATE = re.compile('late+')\n"
    "DEC2 = DEC * 2\n";

/* change between the restored base and the second delta */
static const char change2_src[] =
    "DQ.append(8)\n"
    "next(CNT)\n"
    "LATER = decimal.Decimal('1.5') + DEC\n";

static const char check_src[] =
    "import re, json, itertools, collections, datetime, decimal, array, functools, random\n"
    "import sys, _json\n"
    "failures = []\n"
    "def check(name, ok):\n"
    "    print(('PASS ' if ok else 'FAIL ') + name, flush=True)\n"
    "    if not ok: failures.append(name)\n"
    "def run(name, f):\n"
    "    try:\n"
    "        check(name, f())\n"
    "    except BaseException as e:\n"
    "        print('FAIL ' + name + ': ' + type(e).__name__ + ': ' + str(e), flush=True)\n"
    "        failures.append(name)\n"
    "run('re.Pattern', lambda: type(P) is re.Pattern and isinstance(P, re.Pattern) and P.flags & re.I and P.match('Bob@Example.com').groups() == ('Bob', 'Example'))\n"
    "run('re module works', lambda: re.sub(r'\\d', '#', 'a1b2') == 'a#b#' and re.compile('x+').fullmatch('xxx') is not None)\n"
    "run('re.Pattern is _sre type', lambda: re.Pattern is type(re.compile('a')) and MATCH(P, 'a@b.com') is not None)\n"
    "run('deque', lambda: type(DQ) is collections.deque and CTYPE is collections.deque and DQ.maxlen == 5 and list(DQ) == EXPECT_DQ)\n"
    "run('bound C method', lambda: APPEND.__self__ is DQ and (APPEND(9) or True) and DQ[-1] == 9 and DQ.pop() == 9)\n"
    "run('itertools.count', lambda: next(CNT) == EXPECT_CNT and next(CNT1) == EXPECT_CNT1 and type(CNT) is itertools.count)\n"
    "run('itertools type', lambda: CHAIN is itertools.chain and list(CHAIN([1], [2])) == [1, 2])\n"
    "run('datetime', lambda: DT == datetime.datetime(2024, 5, 17, 12, 30, 15, 123456) and DT.isoformat() == '2024-05-17T12:30:15.123456' and type(DT) is datetime.datetime)\n"
    "run('datetime tz', lambda: DTZ.utcoffset() == datetime.timedelta(0) and DTZ.tzinfo == datetime.timezone.utc and (DTZ - datetime.datetime(2023, 12, 31, tzinfo=datetime.UTC)).days == 1)\n"
    "run('timedelta', lambda: TD.total_seconds() == 3 * 86400 + 7 and TD + TD == datetime.timedelta(days=6, seconds=14))\n"
    "run('decimal', lambda: DEC + 1 == decimal.Decimal('4.14159') and type(DEC) is decimal.Decimal and str(DEC2) == '6.28318')\n"
    "def dec_exc():\n"
    "    try:\n"
    "        decimal.Decimal(1) / decimal.Decimal(0)\n"
    "    except decimal.DivisionByZero:\n"
    "        return True\n"
    "    return False\n"
    "run('decimal C exception class', dec_exc)\n"
    "run('array', lambda: ARR.typecode == 'i' and ARR.tolist() == [1, 2, 3, 5, 8] and (ARR.append(13) or ARR[-1] == 13) and ARR.pop() == 13)\n"
    "run('json.dumps result', lambda: json.loads(JS) == {'a': [1, 2, {'b': None}], 'c': 'x'})\n"
    "run('json works', lambda: json.dumps({'z': [1.5, 'q']}) == '{\"z\": [1.5, \"q\"]}' and json.loads('[1, {\"a\": 2}]') == [1, {'a': 2}])\n"
    "run('C function', lambda: CFUNC is json.decoder.scanstring and CFUNC('\"abc\"', 1) == ('abc', 5) and CFUNC is _json.scanstring)\n"
    "run('defaultdict', lambda: type(DD) is collections.defaultdict and DD['k'] == [1] and DD['new'] == [] and DD.default_factory is list)\n"
    "run('OrderedDict and Counter', lambda: list(OD.items()) == [('a', 1), ('b', 2)] and CTR.most_common(1) == [('a', 5)])\n"
    "run('functools.partial', lambda: PART('101') == 5 and PART.keywords == {'base': 2})\n"
    "def rnd():\n"
    "    st = RND.getstate()\n"
    "    v = RND.random()\n"
    "    RND.setstate(st)\n"
    "    return v == RND_EXPECT and type(RND) is random.Random\n"
    "run('random.Random subclass of a C type', rnd)\n"
    "run('lru_cache wrapper', lambda: square(4) == 16 and square.cache_info().maxsize == 8 and square.__wrapped__(5) == 25)\n"
    "run('Python subclass of a C type', lambda: type(MYDQ) is MyDeque and MYDQ.total() == 11 and isinstance(MYDQ, collections.deque) and MyDeque in collections.deque.__subclasses__())\n"
    "run('C objects as dict keys', lambda: SEEN[decimal.Decimal('3.14159')] == 'decimal key' and SEEN[P] == 'pattern key')\n"
    "run('C module identity', lambda: sys.modules['_json'] is _json and json.decoder.c_scanstring is _json.scanstring)\n"
    "run('new C objects', lambda: collections.deque('ab').popleft() == 'a' and decimal.Decimal('2') ** 10 == 1024 and array.array('d', [1.5]).tolist() == [1.5])\n"
    "import asyncio, csv, threading, logging, subprocess, io, gc, _asyncio\n"
    "run('threading main thread', lambda: threading.current_thread() is threading.main_thread() is MAIN and MAIN.ident == threading.get_ident() and MAIN.native_id == threading.get_native_id() and MAIN.is_alive() and MAIN._os_thread_handle.ident == threading.get_ident())\n"
    "run('main thread with a new ident', lambda: not EXPECT_NEW_IDENT or threading.get_ident() != WRITER_IDENT)\n"
    "def locks():\n"
    "    ok = HELD.locked() and not HELD.acquire(False)\n"
    "    HELD.release(); ok = ok and not HELD.locked(); HELD.acquire()\n"
    "    got = LOCK.acquire(False)\n"
    "    if got: LOCK.release()\n"
    "    ok = ok and got\n"
    "    ok = ok and RL._is_owned() and RL._recursion_count() == 2 and not FREE_RL.locked()\n"
    "    RL.release(); RL.release(); ok = ok and not RL.locked(); RL.acquire(); RL.acquire()\n"
    "    with COND:\n"
    "        COND.notify_all()\n"
    "    return ok and EV.is_set()\n"
    "run('locks held by the main thread', locks)\n"
    "run('thread-local data', lambda: LOCAL.x == 5)\n"
    "def new_thread():\n"
    "    r = []\n"
    "    t = threading.Thread(target=lambda: r.append(threading.get_ident()))\n"
    "    t.start(); t.join()\n"
    "    return len(r) == 1 and r[0] != threading.get_ident() and not DONE.is_alive() and not t.is_alive()\n"
    "run('threads after the restore', new_thread)\n"
    "def csv_dialects():\n"
    "    s = io.StringIO(); csv.writer(s, 'pipes').writerow([1, 'a'])\n"
    "    return s.getvalue() == '\"1\"|\"a\"\\r\\n' and DIALECT is csv.get_dialect('pipes') and sorted(csv.list_dialects()) == ['excel', 'excel-tab', 'pipes', 'unix'] and list(csv.reader(['x,y'])) == [['x', 'y']]\n"
    "run('csv registered dialects', csv_dialects)\n"
    "run('asyncio', lambda: asyncio.run(twice(4)) == 8 and asyncio.Future is _asyncio.Future and len([o for o in gc.get_objects() if isinstance(o, type) and o.__module__ == '_asyncio']) == 4)\n"
    "def log():\n"
    "    buf = io.StringIO(); h = logging.StreamHandler(buf); LOG.addHandler(h)\n"
    "    try:\n"
    "        LOG.warning('w %d', 1)\n"
    "    finally:\n"
    "        LOG.removeHandler(h)\n"
    "    return buf.getvalue() == 'w 1\\n' and logging.getLogger('snap') is LOG\n"
    "run('logging', log)\n"
    "run('subprocess', lambda: (lambda p: p.returncode == 3 and p.stdout == b'out\\n')(subprocess.run(['/bin/sh', '-c', 'echo out; exit 3'], capture_output=True)))\n"
    "if failures:\n"
    "    print('FAILED:', ', '.join(failures), flush=True)\n"
    "    raise SystemExit(1)\n"
    "print('ALL PASSED', flush=True)\n";

static const char *path_base, *path_delta, *path_delta2;

static int write_file(const char *path, const uint8_t *buf, size_t len)
{
    FILE *f = fopen(path, "wb");
    if (!f || fwrite(buf, 1, len, f) != len) {
        perror(path);
        return -1;
    }
    fclose(f);
    return 0;
}

static uint8_t *read_file(const char *path, size_t *plen)
{
    FILE *f = fopen(path, "rb");
    uint8_t *buf;
    long len;
    if (!f) {
        perror(path);
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    len = ftell(f);
    fseek(f, 0, SEEK_SET);
    buf = malloc(len > 0 ? len : 1);
    if (fread(buf, 1, len, f) != (size_t)len) {
        perror(path);
        fclose(f);
        free(buf);
        return NULL;
    }
    fclose(f);
    *plen = len;
    return buf;
}

static void print_exception(JSContext *ctx, const char *what)
{
    JSValue exc = JS_GetException(ctx);
    const char *msg = JS_ToCString(ctx, exc);
    fprintf(stderr, "FAIL %s: %s\n", what, msg ? msg : "?");
    if (msg)
        JS_FreeCString(ctx, msg);
    JS_FreeValue(ctx, exc);
}

static int save(JSContext *ctx, const char *path, int flags)
{
    uint8_t *buf;
    size_t size;
    int ret;

    buf = JS_WriteSnapshot(ctx, &size, NULL, NULL, 0, flags);
    if (!buf) {
        print_exception(ctx, "write snapshot");
        return -1;
    }
    ret = write_file(path, buf, size);
    js_free(ctx, buf);
    printf("snapshot %s: %zu bytes\n", path, size);
    fflush(stdout);
    return ret;
}

/* a C object that cannot be pickled: the write fails with a TypeError
   naming its type, and the session can still be written afterwards */
static int expect_rejected(JSContext *ctx, const char *make, const char *type_name)
{
    char src[512];
    uint8_t *buf;
    size_t size;
    JSValue exc;
    const char *msg;
    int ok;

    snprintf(src, sizeof(src), "TMP = %s\n", make);
    if (qp_embed_exec(ctx, src, "<reject>") != 0)
        return -1;
    buf = JS_WriteSnapshot(ctx, &size, NULL, NULL, 0, 0);
    if (buf) {
        js_free(ctx, buf);
        printf("FAIL unpicklable %s accepted\n", type_name);
        return -1;
    }
    exc = JS_GetException(ctx);
    msg = JS_ToCString(ctx, exc);
    ok = msg && strstr(msg, "TypeError") && strstr(msg, type_name);
    printf("%s unpicklable %s rejected: %s\n", ok ? "PASS" : "FAIL", type_name, msg ? msg : "?");
    if (msg)
        JS_FreeCString(ctx, msg);
    JS_FreeValue(ctx, exc);
    if (qp_embed_exec(ctx, "TMP = None\n", "<reject>") != 0)
        return -1;
    return ok ? 0 : -1;
}

/* a running thread other than the main thread is host state: the write
   fails with a TypeError, and succeeds again once the thread ended */
static int expect_thread_rejected(JSContext *ctx)
{
    uint8_t *buf;
    size_t size;
    JSValue exc;
    const char *msg;
    int ok;

    if (qp_embed_exec(ctx, "LIVE_EV = threading.Event()\n"
                      "LIVE = threading.Thread(target=LIVE_EV.wait)\n"
                      "LIVE.start()\n", "<thread>") != 0)
        return -1;
    buf = JS_WriteSnapshot(ctx, &size, NULL, NULL, 0, 0);
    if (buf) {
        js_free(ctx, buf);
        printf("FAIL running thread accepted\n");
        return -1;
    }
    exc = JS_GetException(ctx);
    msg = JS_ToCString(ctx, exc);
    ok = msg && strstr(msg, "TypeError") && strstr(msg, "thread");
    printf("%s running thread rejected: %s\n", ok ? "PASS" : "FAIL", msg ? msg : "?");
    if (msg)
        JS_FreeCString(ctx, msg);
    JS_FreeValue(ctx, exc);
    if (qp_embed_exec(ctx, "LIVE_EV.set()\nLIVE.join()\nLIVE = LIVE_EV = None\n", "<thread>") != 0)
        return -1;
    return ok ? 0 : -1;
}

static int phase1(void)
{
    JSRuntime *rt = JS_NewRuntime();
    JSContext *ctx = JS_NewContext(rt);

    if (!rt || !ctx || qp_embed_init(ctx) < 0)
        return 1;
    if (qp_embed_exec(ctx, setup_src, "<setup>") != 0)
        return 1;
    if (expect_rejected(ctx, "P.scanner('a b')", "_sre.SRE_Scanner") ||
        expect_rejected(ctx, "_json.make_encoder(None, None, None, None, ': ', ', ', False, False, False)",
                        "_json.Encoder") ||
        expect_rejected(ctx, "P.match('a@b.com')", "re.Match") ||
        expect_thread_rejected(ctx))
        return 1;
    if (save(ctx, path_base, JS_SNAPSHOT_BASE))
        return 1;
    if (qp_embed_exec(ctx, change1_src, "<change1>") != 0)
        return 1;
    if (save(ctx, path_delta, JS_SNAPSHOT_DELTA))
        return 1;
    return 0;
}

static JSContext *restore(JSRuntime *rt, const char *delta_path)
{
    JSSnapshotResult res;
    uint8_t *base, *delta;
    size_t base_len, delta_len;
    char errbuf[512];
    JSContext *ctx;
    int i;

    base = read_file(path_base, &base_len);
    delta = read_file(delta_path, &delta_len);
    if (!base || !delta)
        return NULL;
    if (JS_ReadSnapshot(rt, base, base_len, delta, delta_len, NULL, &res,
                        errbuf, sizeof(errbuf)) < 0) {
        fprintf(stderr, "FAIL read snapshot: %s\n", errbuf);
        return NULL;
    }
    free(base);
    free(delta);
    if (res.context_count < 1) {
        fprintf(stderr, "FAIL no context restored\n");
        return NULL;
    }
    ctx = res.contexts[0];
    for (i = 1; i < res.context_count; i++)
        JS_FreeContext(res.contexts[i]);
    JS_FreeSnapshotResult(rt, &res);
    return ctx;
}

static int run_checks(JSContext *ctx, const char *expect)
{
    if (qp_embed_exec(ctx, expect, "<expect>") != 0)
        return 1;
    return qp_embed_exec(ctx, check_src, "<check>") != 0;
}

static int phase2(void)
{
    JSRuntime *rt = JS_NewRuntime();
    JSContext *ctx = restore(rt, path_delta);

    if (!ctx)
        return 1;
    if (run_checks(ctx, "EXPECT_DQ = [1, 2, 3, 4, 7]\nEXPECT_CNT = 16\nEXPECT_CNT1 = 6\n"
                   "EXPECT_NEW_IDENT = True\n"))
        return 1;
    if (qp_embed_exec(ctx, "TMP = None\nfailures = None\n", "<reset>") != 0 ||
        qp_embed_exec(ctx, change2_src, "<change2>") != 0)
        return 1;
    return save(ctx, path_delta2, JS_SNAPSHOT_DELTA) != 0;
}

static int phase3(void)
{
    JSRuntime *rt = JS_NewRuntime();
    JSContext *ctx = restore(rt, path_delta2);

    if (!ctx)
        return 1;
    if (run_checks(ctx, "EXPECT_DQ = [2, 3, 4, 7, 8]\nEXPECT_CNT = 22\nEXPECT_CNT1 = 7\n"
                   "EXPECT_NEW_IDENT = False\n"))
        return 1;
    return qp_embed_exec(ctx, "assert LATER == decimal.Decimal('4.64159'), LATER\n"
                         "assert LATE.match('lateee')\n"
                         "print('PASS changes of the restored session')\n", "<late>") != 0;
}

struct phase_thread {
    int (*fn)(void);
    int ret;
};

static void *phase_thread_main(void *opaque)
{
    struct phase_thread *pt = opaque;
    pt->ret = pt->fn();
    return NULL;
}

/* fn in a new process; in_thread: in a thread of that process (with the
   stack size of the main thread), the one thread of the runtime */
static int run_phase(int (*fn)(void), const char *name, int in_thread)
{
    pid_t pid;
    int status;

    fflush(stdout);
    fflush(stderr);
    pid = fork();
    if (pid < 0) {
        perror("fork");
        return 1;
    }
    if (pid == 0) {
        int ret;
        if (in_thread) {
            struct phase_thread pt = { fn, 1 };
            pthread_attr_t attr;
            pthread_t th;
            pthread_attr_init(&attr);
            pthread_attr_setstacksize(&attr, 8 << 20);
            if (pthread_create(&th, &attr, phase_thread_main, &pt) != 0 ||
                pthread_join(th, NULL) != 0) {
                perror("pthread");
                _exit(1);
            }
            ret = pt.ret;
        } else {
            ret = fn();
        }
        fflush(stdout);
        fflush(stderr);
        _exit(ret);
    }
    if (waitpid(pid, &status, 0) < 0) {
        perror("waitpid");
        return 1;
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        printf("FAIL %s (status 0x%x)\n", name, status);
        return 1;
    }
    return 0;
}

int main(int argc, char **argv)
{
    char tmpl[] = "/tmp/qpcapisnapXXXXXX";
    char b[64], d[64], d2[64];
    const char *dir = mkdtemp(tmpl);
    int ret;

    if (!dir) {
        perror("mkdtemp");
        return 1;
    }
    snprintf(b, sizeof(b), "%s/base", dir);
    snprintf(d, sizeof(d), "%s/delta", dir);
    snprintf(d2, sizeof(d2), "%s/delta2", dir);
    path_base = b;
    path_delta = d;
    path_delta2 = d2;
    /* the core C modules are registered by the runtime; the launcher ones
       become built-in modules in every process */
    if (PyImport_ExtendInittab(_PyQp_ExtInittab) < 0) {
        fprintf(stderr, "cannot register the C extension modules\n");
        return 1;
    }
    ret = run_phase(phase1, "phase 1 (write)", 0) ||
        run_phase(phase2, "phase 2 (restore in a thread, write a delta of the restored base)", 1) ||
        run_phase(phase3, "phase 3 (restore again)", 0);
    unlink(b);
    unlink(d);
    unlink(d2);
    rmdir(dir);
    if (!ret)
        printf("ALL PHASES PASSED\n");
    return ret;
}
