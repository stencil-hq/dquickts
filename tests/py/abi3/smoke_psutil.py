import os, psutil
p = psutil.Process(os.getpid())
print(p.pid == os.getpid(), p.is_running(), p.status() in ("running", "sleeping"))
print(psutil.cpu_count() >= 1, psutil.cpu_count(logical=False) >= 1)
m = psutil.virtual_memory()
print(m.total > 0, 0 <= m.percent <= 100)
print(type(p.memory_info().rss).__name__, p.memory_info().rss > 0)
print(psutil.pid_exists(os.getpid()), isinstance(psutil.boot_time(), float))
print(sorted(psutil.disk_usage("/")._fields))
try:
    psutil.Process(2**31 - 1)
except psutil.NoSuchProcess as e:
    print("NoSuchProcess", e.pid)
