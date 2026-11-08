# a busy loop does not starve another thread (forced switching)
import _thread, sys, time
print(sys.getswitchinterval())
stop = False
def busy():
    x = 0
    while not stop:
        x += 1
hb = _thread.start_joinable_thread(busy)
t0 = time.monotonic()
done = []
def other():
    s = 0
    for i in range(200000):
        s += i
    done.append(s)
ho = _thread.start_joinable_thread(other)
ho.join()
ok = time.monotonic() - t0 < 5
stop = True
hb.join()
print(done, ok)
sys.setswitchinterval(0.001)
print(sys.getswitchinterval())
for bad in (0, -1.0):
    try:
        sys.setswitchinterval(bad)
    except ValueError as e:
        print('ValueError:', e)
sys.setswitchinterval(0.005)
