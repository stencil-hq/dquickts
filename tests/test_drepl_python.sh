#!/bin/sh
# qjs --drepl FILE --python: a Python session survives restarting qjs.
#
#   tests/test_drepl_python.sh [path/to/qjs]
#
# The commands come from a pipe (the line editor then reads plain lines).

QJS=${1:-./qjs}
dir=$(mktemp -d "${TMPDIR:-/tmp}/drepl_py.XXXXXX") || exit 1
trap 'rm -rf "$dir"' EXIT
snap=$dir/session.snap
out=$dir/out

fail() {
    echo "FAIL: $*"
    echo "--- output:"
    cat "$out"
    exit 1
}

# expect TEXT: the output of the last run has a line containing TEXT
expect() {
    grep -qF -- "$1" "$out" || fail "missing: $1"
}

# run: a session reading the commands on stdin
run() {
    "$QJS" --drepl "$snap" --python > "$out" 2>&1
}

# the same as the process of the function (the target of kill)
exec_run() {
    exec "$QJS" --drepl "$snap" --python > "$out" 2>&1
}

# wait_for TEXT: wait until the output has TEXT (10 s at most)
wait_for() {
    i=0
    while ! grep -qF -- "$1" "$out" 2> /dev/null; do
        i=$((i + 1))
        [ $i -gt 200 ] && return 1
        sleep 0.05
    done
}

# 1. a new session: a function, a class with state, an instance, a
# closure, a generator advanced twice, a module, top-level await
run <<'EOF'
def f(x):
    return x * 2

class Counter:
    def __init__(self):
        self.n = 0
    def inc(self):
        self.n += 1
        return self.n

c = Counter()
c.inc()
def squares():
    for i in range(10):
        yield i * i

g = squares()
next(g)
next(g)
def make_adder(k):
    def add(x):
        return x + k
    return add

add5 = make_adder(5)
import time
f(21)
print("underscore:", _ + 1)
class Tick:
    def __init__(self, v):
        self.v = v
    def __await__(self):
        yield
        return self.v

r = await Tick(41) + 1
print("await:", r)
EOF
[ $? -eq 0 ] || fail "session 1 exit status"
expect "Created the durable session $snap"
expect ">>> 42"
expect "underscore: 43"
expect "await: 42"
[ -s "$snap" ] || fail "no session file"

# 2. restart: everything continues where it was
run <<'EOF'
print("f:", f(5))
print("inc:", c.inc(), c.inc())
print("next:", next(g), next(g))
print("rest:", list(g))
print("add5:", add5(1))
print("time:", type(time.time()).__name__)
print("class:", type(c).__name__, isinstance(c, Counter))
print("await:", r)
1/0
import select
EOF
expect "Restored the durable session $snap"
expect "f: 10"
expect "inc: 2 3"
expect "next: 4 9"
expect "rest: [16, 25, 36, 49, 64, 81]"
expect "add5: 6"
expect "time: float"
expect "class: Counter True"
expect "await: 42"
expect "Traceback (most recent call last):"
expect "ZeroDivisionError: division by zero"

# 3. host state: the snapshot fails, the file keeps the previous session
cp "$snap" "$dir/before"
run <<'EOF'
p = select.poll()
lost = 1
EOF
expect "not saved: TypeError: snapshot: cannot save Python object 'select.poll'"
cmp -s "$snap" "$dir/before" || fail "the session file changed"
run <<'EOF'
print("lost:", "lost" in globals(), "f:", f(1))
EOF
expect "lost: False f: 2"

# 4. Ctrl-C interrupts a running command (KeyboardInterrupt); the
# session goes on and is saved
feed() {
    printf 'n = 0\nprint("spinning", flush=True)\nwhile True:\n    n += 1\n\n'
    wait_for "KeyboardInterrupt" || return
    printf 'try:\n    print("looping", flush=True)\n    while True: pass\nexcept KeyboardInterrupt:\n    print("caught")\n\n'
    wait_for "caught" || return
    printf 'after = n > 0\n'
}
feed | exec_run &
pid=$!
wait_for "spinning" && sleep 0.2 && kill -INT $pid
wait_for "looping" && sleep 0.2 && kill -INT $pid
if ! wait_for "caught"; then
    kill -9 $pid
    fail "Ctrl-C did not interrupt the commands"
fi
wait $pid
expect "KeyboardInterrupt"
run <<'EOF'
print("after:", after, "f:", f(2))
EOF
expect "after: True f: 4"

# 5. SystemExit ends qjs with its status
echo 'raise SystemExit(3)' | run
[ $? -eq 3 ] || fail "SystemExit status"

# 6. a session file only loads in the mode that wrote it
"$QJS" --drepl "$snap" < /dev/null > "$out" 2>&1 && fail "JS mode loaded a Python session"
expect "a Python session (add --python)"
# the durable runtime has no timers (host state)
printf 'let a = 1\nprint("timers:", typeof setTimeout, typeof clearInterval)\n\\q\n' |
    "$QJS" --drepl "$dir/js.snap" > "$out" 2>&1
expect "timers: undefined undefined"
[ -s "$dir/js.snap" ] || fail "no JS session file"
"$QJS" --drepl "$dir/js.snap" --python < /dev/null > "$out" 2>&1 && fail "Python mode loaded a JS session"
expect "not a Python session"
# a JavaScript session cannot switch to Python
printf '\\py\n1 + 1\n' | "$QJS" --drepl "$dir/js.snap" > "$out" 2>&1
expect "\\py: this session has no Python: start it with qjs --drepl FILE --python"
expect "qjs> 2"

# 7. a session that imports asyncio, csv, threading, logging and
# subprocess: the main thread's objects (threading's main Thread, an RLock
# it holds, thread-local data) belong to the main thread of the next qjs,
# the dialects registered with csv stay registered
snap=$dir/modules.snap
run <<'EOF'
import asyncio, csv, threading, logging, subprocess, io
csv.register_dialect('pipes', delimiter='|')
lock = threading.Lock()
rl = threading.RLock()
rl.acquire()
data = threading.local()
data.v = 11
log = logging.getLogger('drepl')
async def twice(x):
    await asyncio.sleep(0)
    return 2 * x

print("run:", asyncio.run(twice(4)))
await asyncio.sleep(0)
done = threading.Thread(target=lambda: None)
done.start()
done.join()
EOF
[ $? -eq 0 ] || fail "session 7 exit status"
expect "run: 8"
grep -qF "not saved" "$out" && fail "session 7 not saved"
run <<'EOF'
r = await asyncio.sleep(0, 'slept')
print("sleep:", r)
print("main:", threading.current_thread() is threading.main_thread(), threading.main_thread().ident == threading.get_ident())
print("rlock:", rl._is_owned(), rl._recursion_count())
rl.release()
print("lock:", lock.acquire(False), data.v, done.is_alive())
s = io.StringIO(); csv.writer(s, 'pipes').writerow(['a', 'b']); print("csv:", repr(s.getvalue()))
print("run:", asyncio.run(twice(5)))
t = threading.Thread(target=lambda: print("thread ran", flush=True)); t.start(); t.join()
log.warning("logged")
print("proc:", subprocess.run(['/bin/sh', '-c', 'exit 3']).returncode)
EOF
expect "Restored the durable session $snap"
expect "sleep: slept"
expect "main: True True"
expect "rlock: True 1"
expect "lock: True 11 False"
expect "csv: 'a|b\r\n'"
expect "run: 10"
expect "thread ran"
expect "logged"
expect "proc: 3"
grep -qF "not saved" "$out" && fail "session 7 not saved after the restart"
run <<'EOF'
print("again:", rl._is_owned(), lock.locked(), sorted(csv.list_dialects()))
EOF
expect "again: False True ['excel', 'excel-tab', 'pipes', 'unix']"

# 8. a session that used the js module: JS objects held by Python, Python
# objects seen from JS and Function.prototype.callKw survive the restart
snap=$dir/js_interop.snap
run <<'EOF'
import js
shop = js.eval("({ items: [], add(name, qty) { return this.items.push({ name, qty }) } })")
shop.add("apples", 3)
class P:
    x = 41

show = js.eval("(o) => o.x + 1")
print("show:", show(P()))
print("timers:", hasattr(js, "setTimeout"), hasattr(js, "setInterval"))
EOF
expect "show: 42"
expect "timers: False False"
grep -qF "not saved" "$out" && fail "session 8 not saved"
run <<'EOF'
shop.add("pears", 2)
print("items:", [i.name for i in shop.items], sum(i.qty for i in shop.items))
print("show:", show(P()))
def g(a, b=2): return a * 10 + b

print("callKw:", js.eval("(f) => f.callKw([1], {b: 5})")(g))
EOF
expect "Restored the durable session $snap"
expect "items: ['apples', 'pears'] 5"
expect "show: 42"
expect "callKw: 15"

# 9. a bilingual session: \js switches to TypeScript, \py back, '\py CODE'
# and '\js CODE' run one input in the other language; js.NAME reads the JS
# globals (top-level let, const and class too) and py is __main__ in JS.
# The JS state is saved with the Python state, and the session restarts
# in the language it was left in.
snap=$dir/bilingual.snap
run <<'EOF'
\js function add(a: number, b: number): number { return a + b }
js.add(2, 3)
def sq(x): return x * x

\js py.sq(7)
\js
let n: number = 3
console.log("n:", n)
\py js.n + 1
class K { v = 9 }
py.y = 1
\py
print("y:", y, "K:", js.K.new().v)
\js
EOF
[ $? -eq 0 ] || fail "session 9 exit status"
expect ">>> 5"
expect ">>> 49"
expect "n: 3"
expect "qjs> 4"
expect "qjs> 1"
expect ">>> y: 1 K: 9"
grep -qF "not saved" "$out" && fail "session 9 not saved"
run <<'EOF'
add(py.sq(3), n)
\py print("again:", js.add(1, 2), sq(4), js.n, y, js.K.new().v)
\py
EOF
expect "Restored the durable session $snap"
expect "qjs> 12"
expect "again: 3 16 3 1 9"
run <<'EOF'
sq(js.add(1, 1))
EOF
expect ">>> 4"
grep -qF "not saved" "$out" && fail "session 9 not saved after the restart"

echo "test_drepl_python: OK"
