# RLock ownership across threads, timeouts, signals only in the main thread
import _thread, signal, time
r = _thread.RLock()
r.acquire(); r.acquire()
out = []
def other():
    out.append(r.acquire(timeout=0.05))
    out.append(r._is_owned())
    try:
        r.release()
    except RuntimeError as e:
        out.append(str(e))
    try:
        signal.signal(signal.SIGUSR1, signal.SIG_IGN)
    except ValueError as e:
        out.append(str(e))
_thread.start_joinable_thread(other).join()
print(out, r._recursion_count())
r.release(); r.release()
_thread.start_joinable_thread(lambda: out.append(r.acquire(blocking=False))).join()
print(out[-1], r.locked())
def intr():
    time.sleep(0.05)
    _thread.interrupt_main()
_thread.start_new_thread(intr, ())
try:
    t0 = time.monotonic()
    while time.monotonic() - t0 < 10:
        pass
    print('not interrupted')
except KeyboardInterrupt:
    print('KeyboardInterrupt in main')
