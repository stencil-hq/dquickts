# The JS environment and the event loop of the Python launchers (qpy,
# `qjs file.py`, `qjs --python`): js.setTimeout() & co are the timers of
# quickjs-libc, the std/os/bjson modules can be imported, the event loop
# runs after __main__ while the other Python threads run.  Run with
#   QJS=./qjs ./qpy tests/py/interop/test_timers.py
# "PASS test_timers.py" is printed by the event loop (after the main
# module), exit status 0; a failure raises (exit 1) or prints nothing.
import js
import os
import signal
import subprocess
import sys
import tempfile
import time

QJS = os.environ.get("QJS", "./qjs")
checks = 0


def check(cond, msg):
    global checks
    checks += 1
    if not cond:
        raise AssertionError(msg)


def run(code, launcher="qpy"):
    """Run code with a launcher; returns (exit status, stdout, stderr)."""
    if launcher == "qpy":
        args = [sys.executable, "-c", code]
    elif launcher == "qjs --python":
        args = [QJS, "--python", "-c", code]
    else:
        f = tempfile.NamedTemporaryFile("w", suffix=".py", delete=False)
        f.write(code)
        f.close()
        args = [QJS, f.name]
    try:
        p = subprocess.run(args, capture_output=True, text=True, timeout=60)
    finally:
        if launcher == "qjs file.py":
            os.unlink(f.name)
    return p.returncode, p.stdout, p.stderr


def test_environment():
    for name in ("setTimeout", "clearTimeout", "setInterval", "clearInterval"):
        f = getattr(js, name)
        check(js.eval("(f) => typeof f")(f) == "function" and f.name == name, name)
    jsos = js.import_module("os")
    check(jsos.platform == sys.platform, "os module")
    check(js.import_module("std").getenv("PATH") == os.environ.get("PATH"), "std module")
    bjson = js.import_module("bjson")
    buf = bjson.write(js.eval("[1, 2]"))
    check(bjson.read(buf, 0, buf.byteLength) == [1, 2], "bjson module")
    check(list(js.scriptArgs) == sys.argv, "scriptArgs")


def test_launchers():
    code = ("import js, time\n"
            "t = time.monotonic()\n"
            "js.setTimeout(lambda: print('fired', time.monotonic() - t >= 0.095), 100)\n"
            "print('main done')\n")
    for launcher in ("qpy", "qjs --python", "qjs file.py"):
        status, out, err = run(code, launcher)
        check((status, out, err) == (0, "main done\nfired True\n", ""),
              f"{launcher}: {status} {out!r} {err!r}")


def test_callback_errors():
    # reported through sys.excepthook, the loop goes on, the status stays 0
    status, out, err = run("import js, sys, atexit\n"
                           "atexit.register(lambda: print('atexit'))\n"
                           "def bad():\n"
                           "    1/0\n"
                           "js.setTimeout(bad, 1)\n"
                           "js.setTimeout(lambda: print('after'), 20)\n")
    check(status == 0 and out == "after\natexit\n", f"error: {status} {out!r}")
    check("Traceback (most recent call last):" in err and "in bad" in err and
          err.endswith("ZeroDivisionError: division by zero\n"), f"traceback: {err!r}")
    status, out, err = run("import js, sys\n"
                           "sys.excepthook = lambda t, v, tb: print('hook', t.__name__, v)\n"
                           "js.setTimeout(lambda: [][0], 1)\n"
                           "js.eval('setTimeout(() => { throw new Error(\"boom\") }, 5)')\n")
    check((status, out, err) == (0, "hook IndexError list index out of range\n"
                                    "hook JsError Error: boom\n", ""),
          f"excepthook: {status} {out!r} {err!r}")
    # sys.exit() in a callback ends the loop with its status
    status, out, err = run("import js, sys, atexit\n"
                           "atexit.register(lambda: print('atexit'))\n"
                           "js.setTimeout(lambda: sys.exit(3), 1)\n"
                           "js.setTimeout(lambda: print('never'), 50)\n", "qjs --python")
    check((status, out, err) == (3, "atexit\n", ""), f"sys.exit: {status} {out!r} {err!r}")
    # no event loop after an uncaught exception or a SystemExit of __main__
    status, out, err = run("import js\n"
                           "js.setTimeout(lambda: print('never'), 1)\n"
                           "raise ValueError('main')\n")
    check(status == 1 and out == "" and err.endswith("ValueError: main\n"),
          f"main error: {status} {out!r}")
    status, out, err = run("import js, sys\n"
                           "js.setTimeout(lambda: print('never'), 1)\n"
                           "sys.exit()\n")
    check((status, out, err) == (0, "", ""), f"main exit: {status} {out!r}")


def test_keyboard_interrupt():
    # SIGINT while the loop waits: KeyboardInterrupt, death by SIGINT
    p = subprocess.Popen([sys.executable, "-c",
                          "import js, atexit\n"
                          "atexit.register(lambda: print('atexit'))\n"
                          "js.setTimeout(lambda: print('waiting', flush=True), 0)\n"
                          "js.setTimeout(lambda: print('never'), 30000)\n"],
                         stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    check(p.stdout.readline() == "waiting\n", "SIGINT: not started")
    time.sleep(0.2)
    p.send_signal(signal.SIGINT)
    out, err = p.communicate(timeout=30)
    check(p.returncode == -signal.SIGINT and out == "atexit\n" and
          err.endswith("KeyboardInterrupt\n"), f"SIGINT: {p.returncode} {out!r} {err!r}")


def test_threads():
    # the main thread releases the GIL while it waits for a timer: a
    # thread counts meanwhile, another one ends a 10 s wait by replacing
    # the timer
    status, out, err = run("import js, threading, time\n"
                           "n = n0 = 0\n"
                           "stop = False\n"
                           "def count():\n"
                           "    global n\n"
                           "    while not stop:\n"
                           "        n += 1\n"
                           "        time.sleep(0.001)\n"
                           "threading.Thread(target=count).start()\n"
                           "def begin():\n"
                           "    global n0\n"
                           "    n0 = n\n"
                           "def end():\n"
                           "    global stop\n"
                           "    stop = True\n"
                           "    print('counted while waiting:', n - n0 >= 10)\n"
                           "js.setTimeout(begin, 0)\n"
                           "js.setTimeout(end, 300)\n")
    check((status, out, err) == (0, "counted while waiting: True\n", ""),
          f"counter thread: {status} {out!r} {err!r}")
    status, out, err = run("import js, threading, time\n"
                           "t0 = time.monotonic()\n"
                           "long = js.setTimeout(lambda: print('long timer'), 10000)\n"
                           "def worker():\n"
                           "    time.sleep(0.05)\n"
                           "    js.clearTimeout(long)\n"
                           "    js.setTimeout(lambda: print('woken', time.monotonic() - t0 < 5), 0)\n"
                           "threading.Thread(target=worker).start()\n")
    check((status, out, err) == (0, "woken True\n", ""),
          f"timer of a thread: {status} {out!r} {err!r}")


# the timers of this process: Python callables, run in the order of their
# expiration after the main module; clearTimeout() cancels
order = []
main_done = False


def finish():
    check(main_done, "the loop runs after the main module")
    check(order == ["t1", "i1", "i2", "i3", "t200", "t210"], f"order {order}")
    print(f"PASS test_timers.py ({checks} checks)")


def tick():
    n = sum(o[0] == "i" for o in order) + 1
    order.append(f"i{n}")
    if n == 3:
        js.clearInterval(interval)


def schedule_timers():
    global interval
    js.setTimeout(lambda: order.append("t210"), 210)
    js.setTimeout(lambda: order.append("t200"), 200)
    cancelled = js.setTimeout(lambda: order.append("cancelled"), 5)
    js.import_module("os").setTimeout(lambda: order.append("t1"), 1)
    js.clearTimeout(cancelled)
    interval = js.setInterval(tick, 10)
    js.setTimeout(finish, 300)


test_environment()
test_launchers()
test_callback_errors()
test_keyboard_interrupt()
test_threads()
schedule_timers()
main_done = True
