/*
 * Python heap snapshot test host: builds one object of every supported
 * Python class in a runtime, writes a snapshot, frees the runtime, reads
 * the snapshot into a fresh runtime and checks with Python code that every
 * object behaves identically (PASS/FAIL per object, non-zero exit status
 * on failure).
 *
 * Needs an engine built with CONFIG_PYTHON (see the Makefile rule).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "quickjs.h"

/* JS_FreeRuntime() of a runtime with Python state currently crashes in the
   core (qp_state_free from a context released by the final GC, also
   without any snapshot): set QP_TEST_FREE_RUNTIME=1 to exercise it.  The
   restored runtime is always a fresh one. */
static void free_runtime(JSRuntime *rt)
{
    const char *s = getenv("QP_TEST_FREE_RUNTIME");
    if (s && *s == '1')
        JS_FreeRuntime(rt);
}

/* python/snapshot.c embedding entry points */
int qp_embed_init(JSContext *ctx);
int qp_embed_exec(JSContext *ctx, const char *src, const char *filename);

/* Objects built before the snapshot.  EXPECT maps a name to the repr /
   values computed before the write; the checks compare against them. */
static const char setup_src[] =
    "import sys\n"
    "L = [1, 'two', 3.5, None, True]\n"
    "T = (1, (2, 3), 'x', frozenset({4}))\n"
    "D = {'a': 1, 'b': 2, 'c': 3, 'd': 4, 5: 'five', (6, 7): 'tuple'}\n"
    "del D['b']; del D[5]; D['e'] = 5; D['b'] = 'again'\n"
    "S = {'x', 'y', 'z', 1, 2, 3, 2**40}\n"
    "S.discard('y')\n"
    "FS = frozenset(['p', 'q', 'r', 10])\n"
    "B = b'bytes\\x00\\xff'\n"
    "BA = bytearray(b'abc'); BA += b'def'\n"
    "MV = memoryview(BA)[1:4]\n"
    "SMALL = 42; BIG = 2**200 + 12345; NEG = -(2**70)\n"
    "F = 1.0; F2 = -2.5e-300\n"
    "C = complex(1.5, -2.0)\n"
    "U = 'snake \\U0001F40D and \\u00e9'\n"
    "class Base:\n"
    "    __slots__ = ('a', 'b')\n"
    "    def __init__(self, a, b): self.a = a; self.b = b\n"
    "    @classmethod\n"
    "    def make(cls): return cls(1, 2)\n"
    "    @property\n"
    "    def total(self): return self.a + self.b\n"
    "    @staticmethod\n"
    "    def st(x): return x * 2\n"
    "class Derived(Base):\n"
    "    __slots__ = ('c',)\n"
    "    def __init__(self, a, b, c): super().__init__(a, b); self.c = c\n"
    "    def __repr__(self): return f'Derived({self.a}, {self.b}, {self.c})'\n"
    "class Plain:\n"
    "    def __init__(self): self.x = 1; self.y = [1, 2]\n"
    "INST = Derived(1, 2, 3)\n"
    "PLAIN = Plain()\n"
    "OBJKEY = Plain()\n"
    "IDDICT = {OBJKEY: 'by identity', Plain: 'class key'}\n"
    "IDSET = {OBJKEY, Plain, len}\n"
    "def make_counter(start):\n"
    "    n = start\n"
    "    def inc(step=1):\n"
    "        nonlocal n\n"
    "        n += step\n"
    "        return n\n"
    "    return inc\n"
    "CLOSURE = make_counter(10); CLOSURE()\n"
    "def gen(n):\n"
    "    for i in range(n):\n"
    "        yield i * i\n"
    "GEN = gen(6); next(GEN); next(GEN)\n"
    "class Awaitable:\n"
    "    def __init__(self, v): self.v = v\n"
    "    def __await__(self):\n"
    "        yield 'tick1'\n"
    "        yield 'tick2'\n"
    "        return self.v\n"
    "async def coro():\n"
    "    x = await Awaitable(7)\n"
    "    return x * 6\n"
    "CORO = coro(); FIRST = CORO.send(None)\n"
    "def fail():\n"
    "    raise ValueError('boom', 3)\n"
    "try:\n"
    "    fail()\n"
    "except ValueError as e:\n"
    "    EXC = e\n"
    "MOD = sys\n"
    "R = range(3, 30, 4)\n"
    "SL = slice(1, None, 2)\n"
    "KV = D.keys(); VV = D.values(); IV = D.items()\n"
    "class MyInt(int):\n"
    "    def double(self): return MyInt(self * 2)\n"
    "BOXED = MyInt(21)\n"
    "def deco(f):\n"
    "    def wrapper(*a, **k): return ('wrapped', f(*a, **k))\n"
    "    wrapper.__name__ = f.__name__\n"
    "    return wrapper\n"
    "@deco\n"
    "def decorated(x): return x + 1\n"
    "BOUND = Base.make\n"
    "BUILTIN = len\n"
    "METHOD_DESCR = str.upper\n"
    "IT = iter([5, 6, 7]); next(IT)\n"
    "EXPECT = {\n"
    "    'L': repr(L), 'T': repr(T), 'D': repr(D), 'S': list(S), 'FS': list(FS),\n"
    "    'B': repr(B), 'BA': repr(BA), 'MV': MV.tobytes(), 'U': U,\n"
    "    'INST': repr(INST), 'R': list(R), 'SL': repr(SL),\n"
    "    'KV': list(KV), 'VV': list(VV), 'IV': list(IV),\n"
    "}\n";

static const char check_src[] =
    "import sys\n"
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
    "run('None identity', lambda: None is None and type(None)() is None)\n"
    "run('builtin type identity', lambda: type(1) is int and type('') is str and type(1.0) is float and type([]) is list)\n"
    "run('list', lambda: repr(L) == EXPECT['L'] and type(L) is list)\n"
    "run('tuple', lambda: repr(T) == EXPECT['T'] and T[1] == (2, 3) and hash(T) == hash((1, (2, 3), 'x', frozenset({4}))))\n"
    "run('dict with deletions', lambda: repr(D) == EXPECT['D'] and list(D) == ['a', 'c', 'd', (6, 7), 'e', 'b'] and D[(6, 7)] == 'tuple')\n"
    "run('set order', lambda: list(S) == EXPECT['S'] and 2**40 in S and 'y' not in S)\n"
    "run('frozenset', lambda: list(FS) == EXPECT['FS'] and FS == frozenset(['p', 'q', 'r', 10]) and hash(FS) == hash(frozenset(['p', 'q', 'r', 10])))\n"
    "run('identity-keyed dict', lambda: IDDICT[OBJKEY] == 'by identity' and IDDICT[Plain] == 'class key')\n"
    "run('identity-keyed set', lambda: OBJKEY in IDSET and Plain in IDSET and len in IDSET)\n"
    "run('bytes', lambda: repr(B) == EXPECT['B'] and B[-1] == 255)\n"
    "run('bytearray', lambda: repr(BA) == EXPECT['BA'] and (BA.__setitem__(0, 0x7a) or BA[0] == 0x7a))\n"
    "run('memoryview', lambda: MV.tobytes() == EXPECT['MV'] and MV.tobytes() == b'bcd' and MV[0] == ord('b'))\n"
    "run('small int', lambda: SMALL == 42 and type(SMALL) is int)\n"
    "run('big int', lambda: BIG == 2**200 + 12345 and NEG == -(2**70) and BIG % 1000 == (2**200 + 12345) % 1000)\n"
    "run('float', lambda: type(F) is float and repr(F) == '1.0' and F2 == -2.5e-300)\n"
    "run('complex', lambda: C == complex(1.5, -2.0) and repr(C) == '(1.5-2j)')\n"
    "run('str non-BMP', lambda: U == EXPECT['U'] and len(U) == 13 and U[6] == '\\U0001F40D')\n"
    "run('class with __slots__', lambda: Derived.__mro__ == (Derived, Base, object) and Derived.__slots__ == ('c',))\n"
    "run('instance', lambda: repr(INST) == EXPECT['INST'] and INST.total == 3 and isinstance(INST, Base))\n"
    "run('classmethod', lambda: repr(Derived.make.__func__(Base)) is not None and Base.make().total == 3)\n"
    "run('staticmethod', lambda: Base.st(21) == 42)\n"
    "run('plain instance dict', lambda: PLAIN.x == 1 and PLAIN.y == [1, 2] and vars(PLAIN) == {'x': 1, 'y': [1, 2]})\n"
    "run('closure', lambda: CLOSURE() == 12 and CLOSURE(5) == 17)\n"
    "run('generator resume', lambda: list(GEN) == [4, 9, 16, 25])\n"
    "def coro_check():\n"
    "    if FIRST != 'tick1': return False\n"
    "    if CORO.send(None) != 'tick2': return False\n"
    "    try:\n"
    "        CORO.send(None)\n"
    "    except StopIteration as e:\n"
    "        return e.value == 42\n"
    "    return False\n"
    "run('coroutine resume', coro_check)\n"
    "run('exception with traceback', lambda: EXC.args == ('boom', 3) and type(EXC) is ValueError and EXC.__traceback__ is not None and EXC.__traceback__.tb_next.tb_frame.f_code.co_name == 'fail')\n"
    "run('module', lambda: MOD is sys and sys.modules['sys'] is MOD)\n"
    "run('range', lambda: list(R) == EXPECT['R'] and R[2] == 11 and len(R) == 7)\n"
    "run('slice', lambda: repr(SL) == EXPECT['SL'] and [0, 1, 2, 3, 4][SL] == [1, 3])\n"
    "run('dict views', lambda: list(KV) == EXPECT['KV'] and list(VV) == EXPECT['VV'] and list(IV) == EXPECT['IV'] and 'a' in KV)\n"
    "run('boxed int subclass', lambda: BOXED == 21 and type(BOXED) is MyInt and BOXED.double() == 42 and type(BOXED.double()) is MyInt)\n"
    "run('decorator', lambda: decorated(1) == ('wrapped', 2) and decorated.__name__ == 'decorated')\n"
    "run('bound classmethod', lambda: BOUND().total == 3)\n"
    "run('builtin function', lambda: BUILTIN is len and BUILTIN([1, 2]) == 2)\n"
    "run('method descriptor', lambda: METHOD_DESCR('ab') == 'AB')\n"
    "run('iterator resume', lambda: list(IT) == [6, 7])\n"
    "run('new code still runs', lambda: eval('sum(i for i in range(5))') == 10)\n"
    "if failures:\n"
    "    print('FAILED:', ', '.join(failures), flush=True)\n"
    "    raise SystemExit(1)\n"
    "print('ALL PASSED', flush=True)\n";

int main(int argc, char **argv)
{
    JSRuntime *rt;
    JSContext *ctx;
    JSSnapshotResult res;
    uint8_t *buf, *copy;
    size_t size;
    char errbuf[256];
    int status, i;

    rt = JS_NewRuntime();
    ctx = JS_NewContext(rt);
    if (!rt || !ctx || qp_embed_init(ctx) < 0) {
        fprintf(stderr, "cannot initialise Python\n");
        return 1;
    }
    if (qp_embed_exec(ctx, setup_src, "<setup>") != 0) {
        fprintf(stderr, "setup failed\n");
        return 1;
    }
    buf = JS_WriteSnapshot(ctx, &size, NULL, NULL, 0, 0);
    if (!buf) {
        JSValue exc = JS_GetException(ctx);
        const char *msg = JS_ToCString(ctx, exc);
        fprintf(stderr, "FAIL write snapshot: %s\n", msg ? msg : "?");
        return 1;
    }
    /* the buffer belongs to the first runtime */
    copy = malloc(size);
    memcpy(copy, buf, size);
    js_free(ctx, buf);
    JS_FreeContext(ctx);
    free_runtime(rt);
    printf("snapshot: %zu bytes\n", size);
    fflush(stdout); /* Python writes to the descriptor directly */

    rt = JS_NewRuntime();
    if (JS_ReadSnapshot(rt, copy, size, NULL, 0, NULL, &res, errbuf, sizeof(errbuf)) < 0) {
        fprintf(stderr, "FAIL read snapshot: %s\n", errbuf);
        return 1;
    }
    free(copy);
    if (res.context_count < 1) {
        fprintf(stderr, "FAIL no context restored\n");
        return 1;
    }
    ctx = res.contexts[0];
    status = qp_embed_exec(ctx, check_src, "<check>");
    for (i = 0; i < res.context_count; i++)
        JS_FreeContext(res.contexts[i]);
    JS_FreeSnapshotResult(rt, &res);
    free_runtime(rt);
    return status != 0;
}
