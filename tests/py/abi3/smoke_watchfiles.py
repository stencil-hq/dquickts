import os, tempfile, threading, time
from watchfiles import watch, Change, DefaultFilter, PythonFilter
from watchfiles._rust_notify import RustNotify
f = DefaultFilter()
print(f(Change.added, "/a/b/.git/x"), f(Change.added, "/a/b/c.py"), PythonFilter()(Change.modified, "/x/y.txt"))
print(Change.added, Change(2), sorted(c.name for c in Change))
d = tempfile.mkdtemp()
def writer():
    time.sleep(0.5)
    with open(os.path.join(d, "new.txt"), "w") as fh:
        fh.write("hello")
t = threading.Thread(target=writer)
t.start()
for changes in watch(d, debounce=200, step=50, rust_timeout=10000, force_polling=True, poll_delay_ms=50):
    print(sorted({(c.name, os.path.basename(p)) for c, p in changes if p != d}))
    break
t.join()
n = RustNotify([d], False, True, 50, True, False)
print(n.watch(100, 50, 200, None))
n.close()
