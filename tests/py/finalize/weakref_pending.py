# At shutdown the value of a WeakValueDictionary of another module dies
# with __main__, which queues the callback of its KeyedRef; the module's
# dictionary, the KeyedRef and the callback then die together in a cycle
# collection before the queue is drained. The queued callback must stay
# valid until the drain releases it. The key is an extension type
# instance (os.DirEntry).
import os
import sys
import types

SRC = """
import weakref
class S:
    pass
d = weakref.WeakValueDictionary()
def new(k):
    o = d[k] = S()
    return o
"""

m = types.ModuleType("weakref_pending_helper")
exec(SRC, m.__dict__)
sys.modules[m.__name__] = m
here = os.path.dirname(os.path.abspath(__file__))
t = m.new(next(os.scandir(here)))
print(len(m.d))
del m
