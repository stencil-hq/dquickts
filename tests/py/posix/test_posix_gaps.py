# POSIX gaps of the posix/select modules (wave 5): PEP 475 signal handling
# in blocking calls, process spawning and waiting, pseudo-terminals,
# scheduling, ids, special files and device numbers.  Output is compared
# with python3.15 (tests/py/posix/run.sh); every line is deterministic and
# the same on macOS and Linux.
import errno
import os
import posixpath
import select
import signal
import stat
import sys
import tempfile
import time

# forked children must not inherit unflushed output
sys.stdout.reconfigure(line_buffering=True)
tmp = tempfile.mkdtemp()


def show(label, fn, *args, **kw):
    try:
        r = fn(*args, **kw)
    except Exception as e:
        print(label, '->', type(e).__name__, getattr(e, 'errno', None) or e)
        return None
    print(label, '->', r)
    return r


def child_exit(code):
    pid = os.fork()
    if pid == 0:
        os._exit(code)
    return pid


# ---- PEP 475: blocking calls run the Python signal handlers on EINTR ----

class Alarm(Exception):
    pass


def raising(signum, frame):
    raise Alarm()


ticks = []


def counting(signum, frame):
    ticks.append(signum)


def interrupted(label, fn):
    signal.signal(signal.SIGALRM, raising)
    signal.setitimer(signal.ITIMER_REAL, 0.2)
    t = time.monotonic()
    try:
        fn()
        res = 'returned'
    except Alarm:
        res = 'Alarm'
    finally:
        signal.setitimer(signal.ITIMER_REAL, 0)
    print('eintr', label, res, time.monotonic() - t < 1.5)


r, w = os.pipe()
interrupted('select', lambda: select.select([], [], [], 3.0))
interrupted('select fds', lambda: select.select([r], [], [], 3.0))
p = select.poll()
p.register(r, select.POLLIN)
interrupted('poll', lambda: p.poll(3000))
interrupted('poll again', lambda: p.poll(3000))
interrupted('sleep', lambda: time.sleep(3.0))
interrupted('read', lambda: os.read(r, 10))
pid = os.fork()
if pid == 0:
    time.sleep(1.0)
    os._exit(0)
interrupted('waitpid', lambda: os.waitpid(pid, 0))
os.waitpid(pid, 0)

# a handler that returns: the call is retried with the remaining timeout
signal.signal(signal.SIGALRM, counting)
signal.setitimer(signal.ITIMER_REAL, 0.05, 0.05)
t = time.monotonic()
print('retry select', select.select([r], [], [], 0.4), time.monotonic() - t >= 0.35)
t = time.monotonic()
print('retry poll', p.poll(400), time.monotonic() - t >= 0.35)
signal.setitimer(signal.ITIMER_REAL, 0)
print('handler ran', len(ticks) > 2)
signal.signal(signal.SIGALRM, signal.SIG_DFL)
os.close(r)
os.close(w)

# ---- os.system / os.spawn* / os.posix_spawn* ----

print('system', os.waitstatus_to_exitcode(os.system('exit 3')), os.system('true'))
print('system bytes', os.waitstatus_to_exitcode(os.system(b'exit 4')))
print('spawnv', os.spawnv(os.P_WAIT, '/bin/sh', ['sh', '-c', 'exit 6']))
print('spawnlp', os.spawnlp(os.P_WAIT, 'sh', 'sh', '-c', 'exit 7'))
pid = os.spawnvpe(os.P_NOWAIT, 'sh', ['sh', '-c', 'exit $CODE'], {'CODE': '8', 'PATH': os.environ['PATH']})
print('spawnvpe nowait', os.waitstatus_to_exitcode(os.waitpid(pid, 0)[1]))


def spawn_wait(fn, *args, **kw):
    pid = fn(*args, **kw)
    return os.waitstatus_to_exitcode(os.waitpid(pid, 0)[1])


print('posix_spawn', spawn_wait(os.posix_spawn, '/bin/sh', ['sh', '-c', 'exit 5'], os.environ))
print('posix_spawnp', spawn_wait(os.posix_spawnp, 'sh', ('sh', '-c', 'exit 9'), os.environ))
print('posix_spawn env', spawn_wait(os.posix_spawn, '/bin/sh', ['sh', '-c', 'exit $X'], {'X': '11'}))
print('posix_spawn env None', spawn_wait(os.posix_spawn, '/bin/sh', [b'sh', '-c', 'exit 12'], None))

out = os.path.join(tmp, 'spawn.out')
fa = [
    (os.POSIX_SPAWN_OPEN, 1, out, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o644),
    (os.POSIX_SPAWN_DUP2, 1, 2),
    (os.POSIX_SPAWN_CLOSE, 0),
]
print('file_actions', spawn_wait(os.posix_spawn, '/bin/sh',
      ['sh', '-c', 'echo out; echo err >&2; read x 2>/dev/null || echo closed'], os.environ,
      file_actions=fa))
with open(out) as f:
    print('file_actions output', f.read().split())
check = 'import os, sys; sys.exit(int(os.%s(0) != os.getpid()))'
print('setpgroup', spawn_wait(os.posix_spawn, sys.executable,
      [sys.executable, '-I', '-c', check % 'getpgid'], os.environ, setpgroup=0))
print('setsid', spawn_wait(os.posix_spawn, sys.executable,
      [sys.executable, '-I', '-c', check % 'getsid'], os.environ, setsid=True))
print('no setsid', spawn_wait(os.posix_spawn, sys.executable,
      [sys.executable, '-I', '-c', check % 'getsid'], os.environ))
print('setsigmask', spawn_wait(os.posix_spawn, '/bin/sh',
      ['sh', '-c', 'kill -USR1 $$; exit 13'], os.environ, setsigmask=[signal.SIGUSR1]))
old = signal.signal(signal.SIGUSR2, signal.SIG_IGN)
print('setsigdef', spawn_wait(os.posix_spawn, '/bin/sh',
      ['sh', '-c', 'kill -USR2 $$; exit 14'], os.environ, setsigdef=[signal.SIGUSR2])
      == -signal.SIGUSR2)
signal.signal(signal.SIGUSR2, old)
print('resetids', spawn_wait(os.posix_spawn, '/bin/sh', ['sh', '-c', 'exit 15'], os.environ,
                             resetids=True, setpgroup=None, scheduler=None))

show('spawn missing', os.posix_spawn, os.path.join(tmp, 'nope'), ['x'], {})
show('spawnp missing', os.posix_spawnp, 'qp-no-such-program-xyz', ['x'], {})
show('argv type', os.posix_spawn, '/bin/sh', 'sh', {})
show('argv empty', os.posix_spawn, '/bin/sh', [], {})
show('argv first empty', os.posix_spawnp, 'sh', [''], {})
show('argv item', os.posix_spawn, '/bin/sh', ['sh', 1], {})
show('env type', os.posix_spawn, '/bin/sh', ['sh'], 1)
show('env list', os.posix_spawn, '/bin/sh', ['sh'], [])
show('env key', os.posix_spawn, '/bin/sh', ['sh'], {'A=B': 'c'})
show('scheduler type', os.posix_spawn, '/bin/sh', ['sh'], {}, scheduler=1)
show('file_actions type', os.posix_spawn, '/bin/sh', ['sh'], {}, file_actions=1)
show('file_actions item', os.posix_spawn, '/bin/sh', ['sh'], {}, file_actions=[[0, 1]])
show('file_actions empty', os.posix_spawn, '/bin/sh', ['sh'], {}, file_actions=[()])
show('file_actions tag', os.posix_spawn, '/bin/sh', ['sh'], {}, file_actions=[(99,)])
show('file_actions open', os.posix_spawn, '/bin/sh', ['sh'], {}, file_actions=[(os.POSIX_SPAWN_OPEN, 1)])
show('file_actions close', os.posix_spawn, '/bin/sh', ['sh'], {}, file_actions=[(os.POSIX_SPAWN_CLOSE,)])
show('file_actions dup2', os.posix_spawn, '/bin/sh', ['sh'], {}, file_actions=[(os.POSIX_SPAWN_DUP2, 1)])
show('setsigmask bad', os.posix_spawn, '/bin/sh', ['sh'], {}, setsigmask=[0])
show('kw only', os.posix_spawn, '/bin/sh', ['sh'], {}, ())
show('posonly', os.posix_spawn, path='/bin/sh', argv=['sh'], env={})
print('spawn constants', os.POSIX_SPAWN_OPEN, os.POSIX_SPAWN_CLOSE, os.POSIX_SPAWN_DUP2)

# ---- wait3 / wait4 / waitid ----

pid = child_exit(7)
rpid, status, ru = os.wait4(pid, 0)
print('wait4', rpid == pid, os.waitstatus_to_exitcode(status), type(ru).__name__,
      type(ru.ru_utime).__name__, type(ru.ru_maxrss).__name__)
pid = child_exit(3)
rpid, status, ru = os.wait3(options=0)
print('wait3', rpid == pid, os.waitstatus_to_exitcode(status), len(ru))
show('wait3 nochild', os.wait3, os.WNOHANG)
show('wait4 nochild', os.wait4, -1, 0)
pid = child_exit(21)
res = os.waitid(os.P_PID, pid, os.WEXITED)
print('waitid', type(res).__name__, res.si_pid == pid, res.si_status, res.si_code == os.CLD_EXITED,
      res.si_signo == signal.SIGCHLD, len(res))
pid = os.fork()
if pid == 0:
    time.sleep(0.3)
    os._exit(0)
print('waitid WNOHANG', os.waitid(os.P_PID, pid, os.WEXITED | os.WNOHANG))
os.waitpid(pid, 0)
show('waitid nochild', os.waitid, os.P_ALL, 0, os.WEXITED)
print('waitid_result', os.waitid_result.__module__, os.waitid_result.n_fields)
print('idtypes', os.P_PID, os.P_PGID, os.P_ALL)
print('CLD', os.CLD_EXITED, os.CLD_KILLED, os.CLD_DUMPED, os.CLD_TRAPPED, os.CLD_STOPPED,
      os.CLD_CONTINUED)

# ---- process groups, sessions and priorities ----

print('getsid', os.getsid(0) == os.getsid(os.getpid()))
print('getpgid', os.getpgid(0) == os.getpgrp())
def in_child(fn):
    r, w = os.pipe()
    pid = os.fork()
    if pid == 0:
        try:
            os.close(r)
            try:
                res = repr(fn())
            except Exception as e:
                res = '%s %s' % (type(e).__name__, getattr(e, 'errno', None))
            os.write(w, res.encode())
        finally:
            os._exit(0)
    os.close(w)
    os.waitpid(pid, 0)
    data = os.read(r, 1000)
    os.close(r)
    return data.decode()


print('child setpgrp', in_child(lambda: (os.setpgrp(), os.getpgrp() == os.getpid())))
print('child setsid', in_child(lambda: (os.setsid(), os.getsid(0) == os.getpid())))
print('child setpgrp+setsid', in_child(lambda: (os.setpgrp(), os.setsid())))
print('child setpgid', in_child(lambda: (os.setpgid(0, 0), os.getpgid(0) == os.getpid())))
prio = os.getpriority(os.PRIO_PROCESS, 0)
print('getpriority', type(prio).__name__, os.nice(0) == prio)
print('setpriority', os.setpriority(os.PRIO_PROCESS, 0, prio))
show('setpriority bad', os.setpriority, os.PRIO_PROCESS, 0, -30 if os.getuid() else 100)
print('PRIO', os.PRIO_PROCESS, os.PRIO_PGRP, os.PRIO_USER)
print('sched', type(os.sched_get_priority_max(os.SCHED_OTHER)).__name__,
      os.sched_get_priority_min(policy=os.SCHED_FIFO) <= os.sched_get_priority_max(os.SCHED_FIFO),
      os.sched_yield())
show('sched_get_priority_max type', os.sched_get_priority_max, 'x')
print('loadavg', [type(x).__name__ for x in os.getloadavg()])

# ---- user and group ids ----

print('setuid', os.setuid(os.getuid()), os.seteuid(os.geteuid()), os.setegid(os.getegid()),
      os.setgid(os.getgid()), os.setreuid(-1, -1), os.setregid(-1, -1))
show('setuid type', os.setuid, 'x')
show('setuid range', os.setuid, 2**40)
for v in (2**32 - 1, 2**32, 2**63, 2**64, -2):
    show('chown uid %d' % v, os.chown, os.path.join(tmp, 'missing'), v, -1)
if os.getuid() != 0:
    show('setgroups', os.setgroups, [os.getgid()])
    show('chroot', os.chroot, tmp)
show('setgroups type', os.setgroups, 1)
show('setgroups item', os.setgroups, ['x'])
show('setgroups many', os.setgroups, list(range(100000)))

# ---- pseudo-terminals ----

master, slave = os.openpty()
print('openpty', os.isatty(master), os.isatty(slave), os.get_inheritable(master),
      os.get_inheritable(slave))
print('ttyname', os.ttyname(slave).startswith('/dev/'), os.ttyname(slave) == os.ptsname(master))
print('terminal_size', tuple(os.get_terminal_size(slave)))
show('ttyname pipe', os.ttyname, os.pipe()[0])
show('get_terminal_size pipe', os.get_terminal_size, os.pipe()[0])
show('tcgetpgrp nottty', os.tcgetpgrp, slave)
os.close(master)
os.close(slave)
fd = os.posix_openpt(os.O_RDWR | os.O_NOCTTY)
print('posix_openpt', os.get_inheritable(fd), os.grantpt(fd), os.unlockpt(fd),
      os.ptsname(fd).startswith('/dev/'))
os.close(fd)
show('grantpt bad', os.grantpt, 9999)
show('ptsname type', os.ptsname, 'x')
print('ctermid', os.ctermid())

def read_line(master, pid, sync_w):
    # the child keeps the slave open until the parent has read its line
    data = b''
    while b'\n' not in data:
        try:
            chunk = os.read(master, 100)
        except OSError:
            break
        if not chunk:
            break
        data += chunk
    os.close(sync_w)
    os.waitpid(pid, 0)
    os.close(master)
    return data.replace(b'\r', b'')


sync_r, sync_w = os.pipe()
pid, master = os.forkpty()
if pid == 0:
    try:
        os.close(sync_w)
        os.write(1, b'in child %d %d %d %d\n' % (os.isatty(0), os.isatty(1),
                 os.getsid(0) == os.getpid(), os.tcgetpgrp(0) == os.getpgrp()))
        os.read(sync_r, 1)
    finally:
        os._exit(0)
os.close(sync_r)
print('forkpty', os.get_inheritable(master))
print('forkpty child', read_line(master, pid, sync_w))

sync_r, sync_w = os.pipe()
master, slave = os.openpty()
pid = os.fork()
if pid == 0:
    try:
        os.close(master)
        os.close(sync_w)
        os.login_tty(slave)
        os.write(1, b'tty %d %d\n' % (os.isatty(0), os.getsid(0) == os.getpid()))
        os.read(sync_r, 1)
    finally:
        os._exit(0)
os.close(slave)
os.close(sync_r)
print('login_tty child', read_line(master, pid, sync_w))

# ---- files: truncate, lockf, fifo, nodes, statvfs, vectored I/O ----

path = os.path.join(tmp, 'f')
with open(path, 'wb') as f:
    f.write(b'hello world')
os.truncate(path, 5)
print('truncate', os.stat(path).st_size)
fd = os.open(path, os.O_RDWR)
os.ftruncate(fd, 3)
print('ftruncate', os.fstat(fd).st_size)
print('lockf', os.lockf(fd, os.F_LOCK, 0), os.lockf(fd, os.F_TEST, 0), os.lockf(fd, os.F_ULOCK, 0),
      os.lockf(fd, os.F_TLOCK, 0))
show('lockf bad', os.lockf, 9999, os.F_LOCK, 0)
print('F_', os.F_ULOCK, os.F_LOCK, os.F_TLOCK, os.F_TEST)
print('pwritev', os.pwritev(fd, [b'ab', bytearray(b'cd'), memoryview(b'ef')], 2))
bufs = [bytearray(3), bytearray(10)]
print('preadv', os.preadv(fd, bufs, 1), bufs)
show('preadv type', os.preadv, fd, 1, 0)
show('preadv ro', os.preadv, fd, [b'x'], 0)
show('pwritev type', os.pwritev, fd, None, 0)
os.close(fd)
print('statvfs', type(os.statvfs(tmp)).__name__, os.statvfs(tmp).f_bsize > 0)
print('sync', os.sync())

fifo = os.path.join(tmp, 'fifo')
os.mkfifo(fifo)
print('mkfifo', stat.S_ISFIFO(os.stat(fifo).st_mode), oct(stat.S_IMODE(os.stat(fifo).st_mode) & 0o600))
show('mkfifo exists', os.mkfifo, fifo)
dfd = os.open(tmp, os.O_RDONLY)
os.mkfifo('fifo2', 0o600, dir_fd=dfd)
print('mkfifo dir_fd', stat.S_ISFIFO(os.stat('fifo2', dir_fd=dfd).st_mode))
os.mknod('node', stat.S_IFIFO | 0o600, dir_fd=dfd)
print('mknod dir_fd', stat.S_ISFIFO(os.stat(os.path.join(tmp, 'node')).st_mode))
os.close(dfd)
os.mknod(os.path.join(tmp, 'node2'), stat.S_IFIFO | 0o644, 0)
print('mknod', stat.S_ISFIFO(os.stat(os.path.join(tmp, 'node2')).st_mode))
show('mknod exists', os.mknod, fifo, stat.S_IFIFO | 0o600)
print('supports_dir_fd', os.mkfifo in os.supports_dir_fd, os.mknod in os.supports_dir_fd)
d = os.makedev(8, 17)
print('makedev', os.major(d), os.minor(d), os.major(os.makedev(0, 0)), type(d).__name__)
show('makedev neg', os.makedev, -2, 0)
show('makedev big', os.makedev, 2**40, 0)
show('major type', os.major, 1.5)
st = os.stat(tmp)
print('st_dev', os.makedev(os.major(st.st_dev), os.minor(st.st_dev)) == st.st_dev)

# ---- sendfile ----
import socket
a, b = socket.socketpair()
with open(path, 'wb') as f:
    f.write(b'0123456789')
fd = os.open(path, os.O_RDONLY)
n = os.sendfile(a.fileno(), fd, 2, 5)
print('sendfile', n, b.recv(100))
n = os.sendfile(out_fd=a.fileno(), in_fd=fd, offset=0, count=3)
print('sendfile kw', n, b.recv(100))
show('sendfile neg', os.sendfile, a.fileno(), fd, 0, -1)
show('sendfile bad fd', os.sendfile, a.fileno(), 9999, 0, 1)
os.close(fd)
a.close()
b.close()

# ---- pipe2 ----
if hasattr(os, 'pipe2'):
    r, w = os.pipe2(os.O_NONBLOCK | os.O_CLOEXEC)
    print('pipe2', os.get_blocking(r), os.get_inheritable(w))
    os.close(r)
    os.close(w)
else:
    print('pipe2', False, False)
r, w = os.pipe()
print('pipe', os.get_inheritable(r), os.get_inheritable(w), os.get_blocking(r))
os.close(r)
os.close(w)

# ---- posix path helpers ----
import posix
for p in ['', '.', '/', '//', '///', 'a//b/./c/..', '/../x', '../../a/./b', './',
          '//a/b/../..', 'a/b/', '\udc80/./x', 'a\x00b/../c', 'é/../ü//']:
    print('normpath', repr(p), repr(posix._path_normpath(p)), repr(posixpath.normpath(p)),
          repr(posix._path_splitroot_ex(p)))
print('normpath bytes', posix._path_normpath(b'//a/../b/.'), posix._path_splitroot_ex(b'///x'))
print('splitroot pathlike', posix._path_splitroot_ex(__import__('pathlib').PurePosixPath('//x/y')))
show('normpath type', posix._path_normpath, 1)
show('splitroot type', posix._path_splitroot_ex, None)

# ---- environment ----
os.putenv('QP_POSIX_GAPS', 'yes')
print('environ before', 'QP_POSIX_GAPS' in os.environ)
os.reload_environ()
print('environ after', os.environ.get('QP_POSIX_GAPS'))
print('_create_environ', type(posix._create_environ()).__name__,
      posix._create_environ()[b'QP_POSIX_GAPS'])
print('inputhook', posix._is_inputhook_installed(), posix._inputhook())

# ---- constants ----
print('EX', os.EX_OK, os.EX_USAGE, os.EX_SOFTWARE, os.EX_TEMPFAIL, os.EX_CONFIG)
print('RTLD', os.RTLD_LAZY, os.RTLD_NOW > 0, os.RTLD_GLOBAL > 0, hasattr(os, 'RTLD_LOCAL'))
print('SCHED', os.SCHED_OTHER, os.SCHED_FIFO, os.SCHED_RR)
names = ['system', 'posix_spawn', 'posix_spawnp', 'wait3', 'wait4', 'waitid', 'getloadavg',
         'sched_get_priority_max', 'sched_get_priority_min', 'sched_yield', 'openpty', 'forkpty',
         'login_tty', 'posix_openpt', 'grantpt', 'unlockpt', 'ptsname', 'getpriority', 'setpriority',
         'statvfs', 'sync', 'truncate', 'lockf', 'chroot', 'mkfifo', 'mknod', 'major', 'minor',
         'makedev', 'ttyname', 'ctermid', 'getsid', 'setsid', 'setpgid', 'getpgid', 'tcgetpgrp',
         'tcsetpgrp', 'nice', 'sendfile', 'preadv', 'pwritev', 'setuid', 'seteuid', 'setegid',
         'setgid', 'setreuid', 'setregid', 'setgroups', 'getgrouplist', 'initgroups', 'setpgrp',
         'reload_environ', 'spawnv', 'spawnve', 'spawnvp', 'spawnvpe', 'spawnl', 'spawnle',
         'spawnlp', 'spawnlpe']
print('missing', [n for n in names if not hasattr(os, n)])

# last: macOS getgrouplist()/initgroups() start Open Directory threads (a
# later fork() would warn about them in CPython)
print('getgrouplist', 0 in os.getgrouplist('root', 0), os.getgrouplist('root', 12345)[0])
show('getgrouplist type', os.getgrouplist, b'root', 0)
if os.getuid() != 0:
    show('initgroups', os.initgroups, 'root', 0)
print('done')
