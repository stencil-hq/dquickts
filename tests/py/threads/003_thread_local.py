# _thread._local: one dict per thread, __init__ re-run, cleanup at exit
import _thread, _weakref
loc = _thread._local()
loc.x = 'main'
seen = []
def f(n):
    seen.append(('before', hasattr(loc, 'x')))
    loc.x = n
    seen.append(('after', loc.x, sorted(vars(loc))))
for n in range(3):
    _thread.start_joinable_thread(lambda n=n: f(n)).join()
print(sorted(seen, key=repr))
print(loc.x, loc.__dict__)
class MyLocal(_thread._local):
    def __init__(self, v):
        self.v = v
        self.tid = _thread.get_ident()
ml = MyLocal(5)
res = []
_thread.start_joinable_thread(lambda: res.append((ml.v, ml.tid == _thread.get_ident()))).join()
print(res, ml.tid == _thread.get_ident())
class Value: pass
refs = []
def keep():
    v = Value()
    loc.v = v
    refs.append(_weakref.ref(v))
_thread.start_joinable_thread(keep).join()
print('released at thread exit:', refs[0]() is None, hasattr(loc, 'v'))
try:
    _thread._local(1)
except TypeError as e:
    print('TypeError:', e)
