# Condition / Semaphore / Barrier built on _thread locks
import _thread
class Condition:
    def __init__(self):
        self.lock = _thread.allocate_lock(); self.waiters = []
    def __enter__(self): self.lock.acquire()
    def __exit__(self, *a): self.lock.release()
    def wait(self):
        w = _thread.allocate_lock(); w.acquire(); self.waiters.append(w)
        self.lock.release()
        try:
            w.acquire()
        finally:
            self.lock.acquire()
    def notify(self, n=1):
        for w in self.waiters[:n]:
            w.release()
        del self.waiters[:n]
    def notify_all(self): self.notify(len(self.waiters))
class Semaphore:
    def __init__(self, v): self.c = Condition(); self.v = v
    def acquire(self):
        with self.c:
            while self.v == 0:
                self.c.wait()
            self.v -= 1
    def release(self):
        with self.c:
            self.v += 1
            self.c.notify()
class Barrier:
    def __init__(self, n): self.c = Condition(); self.n = n; self.count = 0; self.gen = 0
    def wait(self):
        with self.c:
            gen = self.gen; self.count += 1
            if self.count == self.n:
                self.gen += 1; self.count = 0; self.c.notify_all(); return True
            while gen == self.gen:
                self.c.wait()
            return False
sem = Semaphore(2); inside = 0; peak = 0; mx = _thread.allocate_lock()
bar = Barrier(5); phases = []; leaders = []
def worker(i):
    global inside, peak
    for _ in range(20):
        sem.acquire()
        with mx:
            inside += 1; peak = max(peak, inside)
        with mx:
            inside -= 1
        sem.release()
    phases.append(('p1', i))
    if bar.wait():
        leaders.append(i)
    phases.append(('p2', i))
hs = [_thread.start_joinable_thread(lambda i=i: worker(i)) for i in range(5)]
for h in hs:
    h.join()
print('peak <= 2:', peak <= 2, 'leaders:', len(leaders))
print('all p1 before p2:', all(p == 'p1' for p, _ in phases[:5]), sorted(phases))
