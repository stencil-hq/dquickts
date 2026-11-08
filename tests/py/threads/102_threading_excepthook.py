# exceptions in Thread.run go to threading.excepthook
import threading
seen = []
threading.excepthook = lambda a: seen.append((a.exc_type.__name__, str(a.exc_value), a.thread.name))
def bad(): raise RuntimeError('in thread')
t = threading.Thread(target=bad, name='T1'); t.start(); t.join()
print(seen)
