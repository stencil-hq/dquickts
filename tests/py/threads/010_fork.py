# os.fork() in a threaded process: only the forking thread survives
import _thread, os, warnings
never = _thread.allocate_lock(); never.acquire()
_thread.start_new_thread(never.acquire, ())
with warnings.catch_warnings(record=True) as w:
    warnings.simplefilter('always')
    pid = os.fork()
    if pid == 0:
        r = []
        _thread.start_joinable_thread(lambda: r.append(_thread._count())).join()
        print('child:', r, _thread._count(), flush=True)
        os._exit(7)
    _, status = os.waitpid(pid, 0)
print('child status', os.waitstatus_to_exitcode(status))
print([x.category.__name__ for x in w], 'multi-threaded' in str(w[0].message))
