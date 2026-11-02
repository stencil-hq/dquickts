# C containers (C API objects) in the globals of __main__ hold objects with
# __del__. The class and the functions refer to the globals, so these form
# a cycle through the C objects: it is collected at exit and every __del__
# runs (the order of a collection is not compared).
import collections
import functools
import sys

pending = set()
finalized = []


class R:
    def __init__(self, name):
        self.name = name
        pending.add(name)

    def __del__(self):
        pending.discard(self.name)
        finalized.append(self.name)
        if not pending:
            print("finalized:", ", ".join(sorted(finalized)), file=sys.stderr)


def f(*args, **kwargs):
    pass


dq = collections.deque([R("deque")])
pa = functools.partial(f, R("partial argument"), kw=R("partial keyword"))
dc = {"key": collections.deque([R("dict of deques")])}
od = collections.OrderedDict(key=R("OrderedDict"))
dd = collections.defaultdict(list, key=R("defaultdict"))
owner = R("deque holding its owner")
owner.dq = collections.deque([owner])
print("end")
