# Condition, Semaphore, Barrier, Event, Timer
import threading
cond = threading.Condition(); items = []
def consumer(out):
    with cond:
        cond.wait_for(lambda: len(items) >= 3)
        out.append(list(items))
out = []
c = threading.Thread(target=consumer, args=(out,)); c.start()
for i in range(3):
    with cond:
        items.append(i); cond.notify_all()
c.join(); print(out)
sem = threading.BoundedSemaphore(2); bar = threading.Barrier(4); ids = []
def w(i):
    with sem:
        pass
    ids.append(bar.wait())
ts = [threading.Thread(target=w, args=(i,)) for i in range(4)]
for t in ts: t.start()
for t in ts: t.join()
print(sorted(ids))
ev = threading.Event()
tm = threading.Timer(0.05, ev.set); tm.start()
print(ev.wait(5), ev.is_set()); tm.join()
