# time.sleep releases the GIL: two 0.2 s sleeps in parallel take ~0.2 s
import _thread, time
t0 = time.monotonic()
hs = [_thread.start_joinable_thread(lambda: time.sleep(0.2)) for _ in range(2)]
for h in hs:
    h.join()
dt = time.monotonic() - t0
print('parallel sleeps:', 0.19 < dt < 0.35)
