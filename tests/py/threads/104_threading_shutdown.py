# the interpreter waits for non-daemon threads at exit, not for daemon ones
import threading, time
def late():
    time.sleep(0.2)
    print('non-daemon thread finished', flush=True)
threading.Thread(target=late).start()
threading.Thread(target=lambda: threading.Event().wait(), daemon=True).start()
print('main done', flush=True)
