# Cycles through C containers (C API objects) holding objects with __del__
# are collected by gc.collect(), which runs the __del__ methods. In the
# globals of a dropped module, the class with __del__ and the functions
# are themselves in the cycle.
import collections
import functools
import gc
import types


class R:
    def __init__(self, name):
        self.name = name

    def __del__(self):
        print("del", self.name)


class Box:
    pass


def f(*args, **kwargs):
    pass


b = Box()
b.dq = collections.deque([R("deque"), b])
del b
gc.collect()
print("collected: deque in a cycle")

b = Box()
b.pa = functools.partial(f, R("partial"), b)
del b
gc.collect()
print("collected: partial in a cycle")

r = R("deque holding its owner")
r.dq = collections.deque([r])
del r
gc.collect()
print("collected: deque holding its owner")

dq = collections.deque()
dq.append(dq)
dq.append(R("deque holding itself"))
del dq
gc.collect()
print("collected: deque holding itself")

SRC = """
import collections, functools
class R:
    def __init__(self, name):
        self.name = name
    def __del__(self):
        print("del", self.name)
def f(*args, **kwargs):
    pass
"""

for name, code in [
    ("deque", "c = collections.deque([R('module deque')])"),
    ("partial", "c = functools.partial(f, k=R('module partial'))"),
    ("dict of deques", "c = {'k': collections.deque([R('module dict of deques')])}"),
]:
    m = types.ModuleType("capi_cycles_helper")
    exec(SRC + code, m.__dict__)
    del m
    gc.collect()
    print("collected: module globals with a", name)
