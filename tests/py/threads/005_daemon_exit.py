# daemon threads do not keep the process alive; _shutdown joins non-daemon ones
import _thread, time
started = _thread.allocate_lock(); started.acquire()
never = _thread.allocate_lock(); never.acquire()
def blocked():
    started.release()
    never.acquire()
_thread.start_new_thread(blocked, ())
started.acquire()
def busy():
    while True:
        pass
_thread.start_joinable_thread(busy, daemon=True)
out = []
def late():
    time.sleep(0.2)
    out.append('non-daemon done')
h = _thread.start_joinable_thread(late, daemon=False)
_thread._shutdown()
print(out, h.is_done())
print('main exits')
