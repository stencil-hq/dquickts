# Modules still alive at exit (here held by sys) have their globals set to
# None, the modules imported last first, the names in order. A C container
# (C API object) whose last reference goes is freed at once: the __del__
# of what it holds and the weakref callbacks run before the next name is
# cleared, while the modules that the standard streams use still work.
import sys
import types

SRC = """
import collections, functools, sys, weakref
class R:
    def __init__(self, name):
        self.name = name
    def __del__(self, write=sys.stderr.write):
        write("del %s, label %s\\n" % (self.name, label))
def f(*args):
    pass
def callback(ref, write=sys.stderr.write, name=__name__):
    write("weakref callback %s\\n" % name)
dq = collections.deque([R(__name__ + " deque")])
label = __name__ + " label"
pa = functools.partial(f, R(__name__ + " partial"))
dc = {"key": collections.deque([R(__name__ + " dict of deques")])}
ref = weakref.ref(dc["key"][0], callback)
"""

for name in ("capi_module_clear_a", "capi_module_clear_b"):
    m = types.ModuleType(name)
    sys.modules[name] = m
    exec(SRC, m.__dict__)
    setattr(sys, name, m)
del m
print("end")
