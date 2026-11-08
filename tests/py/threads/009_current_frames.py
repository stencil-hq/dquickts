# sys._current_frames / _current_exceptions / per-thread exception state
import _thread, sys, contextvars
ready = _thread.allocate_lock(); ready.acquire()
block = _thread.allocate_lock(); block.acquire()
def waiter():
    try:
        raise KeyError('in thread')
    except KeyError:
        ready.release()
        block.acquire()
h = _thread.start_joinable_thread(waiter)
ready.acquire()
fr = sys._current_frames()
print(len(fr), fr[h.ident].f_code.co_name, fr[_thread.get_ident()].f_code.co_name)
ex = sys._current_exceptions()
print(repr(ex[h.ident]), ex[_thread.get_ident()])
block.release(); h.join()
res = []
try:
    raise ValueError('main')
except ValueError:
    _thread.start_joinable_thread(lambda: res.append(sys.exception())).join()
    print(repr(sys.exception()), res)
cv = contextvars.ContextVar('cv', default='default')
cv.set('main')
_thread.start_joinable_thread(lambda: res.append(cv.get())).join()
print(res[-1], cv.get())
def gen():
    yield 1
    yield 2
g = gen()
next(g)
_thread.start_joinable_thread(lambda: res.append(next(g))).join()
print(res[-1])
def rec(n):
    return 0 if n == 0 else 1 + rec(n - 1)
def deep():
    try:
        rec(10 ** 6)
    except RecursionError:
        res.append('RecursionError')
_thread.start_joinable_thread(deep).join()
print(res[-1], rec(500))
