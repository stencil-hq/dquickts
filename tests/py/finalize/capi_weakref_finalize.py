# weakref.finalize on objects held by C containers (C API objects): called
# when a cycle through the container is collected, and at exit (the alive
# ones with atexit set, the last registered first).
import collections
import functools
import gc
import weakref


class R:
    pass


class Box:
    pass


def report(name):
    print("finalize", name)


b = Box()
b.dq = collections.deque([R(), b])
weakref.finalize(b.dq[0], report, "cycle through a deque")
del b
gc.collect()
print("collected")

dq = collections.deque([R()])
weakref.finalize(dq[0], report, "object in a deque")
pa = functools.partial(report, R())
weakref.finalize(pa.args[0], report, "object in a partial")
dc = {"key": collections.deque([R()])}
weakref.finalize(dc["key"][0], report, "object in a dict of deques")
quiet = collections.deque([R()])
weakref.finalize(quiet[0], report, "not at exit").atexit = False
print("end")
