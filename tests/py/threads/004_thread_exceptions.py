# uncaught exceptions in threads: unraisable hook, _thread._excepthook
import _thread, sys, io
got = []
sys.unraisablehook = lambda u: got.append((u.exc_type.__name__, str(u.exc_value), u.err_msg))
class Boom:
    def __repr__(self): return '<Boom>'
    def __call__(self): raise ValueError('boom')
h = _thread.start_joinable_thread(Boom())
h.join()
_thread.start_joinable_thread(lambda: _thread.exit()).join()
print(got)
out = []
def hook():
    try:
        1 / 0
    except ZeroDivisionError:
        args = _thread._ExceptHookArgs(sys.exc_info() + (None,))
    old = sys.stderr
    sys.stderr = buf = io.StringIO()
    try:
        _thread._excepthook(args)
    finally:
        sys.stderr = old
    out.append(buf.getvalue())
_thread.start_joinable_thread(hook).join()
text = out[0]
print(text.splitlines()[0].startswith('Exception in thread '))
print(text.splitlines()[-1])
