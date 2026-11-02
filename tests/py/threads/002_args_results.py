# thread arguments and results passed back through a lock-based queue
import _thread
class Queue:
    def __init__(self):
        self.items = []
        self.mutex = _thread.allocate_lock()
        self.avail = _thread.allocate_lock()
        self.avail.acquire()
    def put(self, x):
        with self.mutex:
            self.items.append(x)
            if len(self.items) == 1 and self.avail.locked():
                self.avail.release()
    def get(self):
        while True:
            self.avail.acquire()
            with self.mutex:
                if self.items:
                    x = self.items.pop(0)
                    if self.items:
                        self.avail.release()
                    return x
q = Queue()
def square(i, scale=1):
    q.put((i, i * i * scale))
for i in range(10):
    _thread.start_new_thread(square, (i,), {'scale': 2})
print(sorted(q.get() for _ in range(10)))
h = _thread.start_joinable_thread(lambda: q.put(_thread.get_ident()))
ident = q.get()
h.join()
print(ident == h.ident, ident != _thread.get_ident())
for bad in [(1, ()), (print, [])]:
    try:
        _thread.start_new_thread(*bad)
    except TypeError as e:
        print('TypeError:', e)
