# many threads incrementing a shared counter under a lock
import _thread
N, M = 8, 3000
lock = _thread.allocate_lock()
counter = 0
def worker():
    global counter
    for _ in range(M):
        with lock:
            counter += 1
handles = [_thread.start_joinable_thread(worker) for _ in range(N)]
for h in handles:
    h.join()
print(counter, counter == N * M, all(h.is_done() for h in handles), _thread._count())
l = _thread.allocate_lock()
print(l.acquire(), l.acquire(False), l.acquire(timeout=0.01), l.locked())
l.release()
try:
    l.release()
except RuntimeError as e:
    print('RuntimeError:', e)
