# macOS-only parts of the posix gaps (run.sh skips *_darwin.py elsewhere):
# chflags / lchflags, _fcopyfile (shutil's fast copy), sendfile headers
# and trailers, the PRIO_DARWIN_* constants.
import os
import posix
import shutil
import socket
import stat
import tempfile

tmp = tempfile.mkdtemp()
path = os.path.join(tmp, 'f')
with open(path, 'wb') as f:
    f.write(b'0123456789')


def show(label, fn, *args, **kw):
    try:
        r = fn(*args, **kw)
    except Exception as e:
        print(label, '->', type(e).__name__, getattr(e, 'errno', None) or e)
        return None
    print(label, '->', r)
    return r


os.chflags(path, stat.UF_NODUMP)
print('chflags', os.stat(path).st_flags & stat.UF_NODUMP == stat.UF_NODUMP)
os.chflags(path, 0, follow_symlinks=True)
print('chflags clear', os.stat(path).st_flags & stat.UF_NODUMP)
link = os.path.join(tmp, 'l')
os.symlink(path, link)
os.lchflags(link, stat.UF_NODUMP)
print('lchflags', os.lstat(link).st_flags & stat.UF_NODUMP == stat.UF_NODUMP,
      os.stat(path).st_flags & stat.UF_NODUMP)
os.chflags(link, stat.UF_HIDDEN, follow_symlinks=False)
print('chflags nofollow', os.lstat(link).st_flags & stat.UF_HIDDEN == stat.UF_HIDDEN)
show('chflags missing', os.chflags, os.path.join(tmp, 'missing'), 0)
show('chflags type', os.chflags, path, 'x')
show('chflags float', os.chflags, path, 1.5)
print('supports_follow_symlinks', os.chflags in os.supports_follow_symlinks)

fdin = os.open(path, os.O_RDONLY)
dst = os.path.join(tmp, 'copy')
fdout = os.open(dst, os.O_WRONLY | os.O_CREAT, 0o600)
print('_fcopyfile', posix._fcopyfile(fdin, fdout, posix._COPYFILE_DATA))
os.close(fdin)
os.close(fdout)
with open(dst, 'rb') as f:
    print('copied', f.read())
show('_fcopyfile bad', posix._fcopyfile, 9999, 9998, posix._COPYFILE_DATA)
print('COPYFILE', posix._COPYFILE_DATA, posix._COPYFILE_STAT, posix._COPYFILE_ACL,
      posix._COPYFILE_XATTR)
shutil.copyfile(path, os.path.join(tmp, 'copy2'))
with open(os.path.join(tmp, 'copy2'), 'rb') as f:
    print('shutil.copyfile', f.read(), shutil._HAS_FCOPYFILE)

a, b = socket.socketpair()
fd = os.open(path, os.O_RDONLY)
n = os.sendfile(a.fileno(), fd, 0, 4, headers=[b'<', b'h>'], trailers=(b'</t>',))
print('sendfile hdtr', n, b.recv(100))
n = os.sendfile(a.fileno(), fd, 7, 0)
print('sendfile to eof', n, b.recv(100))
show('sendfile headers type', os.sendfile, a.fileno(), fd, 0, 1, headers=1)
show('sendfile trailers type', os.sendfile, a.fileno(), fd, 0, 1, trailers=1)
show('sendfile headers item', os.sendfile, a.fileno(), fd, 0, 1, headers=[1])
os.close(fd)
a.close()
b.close()
print('PRIO_DARWIN', os.PRIO_DARWIN_THREAD, os.PRIO_DARWIN_PROCESS, os.PRIO_DARWIN_BG,
      os.PRIO_DARWIN_NONUI)
print('NODEV', os.NODEV, os.major(os.NODEV), os.minor(os.NODEV))
print('pipe2', hasattr(os, 'pipe2'))
