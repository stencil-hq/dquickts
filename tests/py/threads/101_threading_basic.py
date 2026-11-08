# threading.Thread with args, results through queue.Queue (needs itertools)
import threading, queue
q = queue.Queue()
def work(i, scale=1):
    q.put((i, i * scale, threading.current_thread().name))
ts = [threading.Thread(target=work, args=(i,), kwargs={'scale': 3}, name=f'w{i}') for i in range(6)]
for t in ts: t.start()
for t in ts: t.join()
print(sorted(q.get() for _ in range(6)))
print([t.is_alive() for t in ts], threading.active_count(), threading.current_thread() is threading.main_thread())
loc = threading.local(); loc.v = 'main'
r = []
t = threading.Thread(target=lambda: r.append(getattr(loc, 'v', None))); t.start(); t.join()
print(r, loc.v)
