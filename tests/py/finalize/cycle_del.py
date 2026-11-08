import sys

class Node:
    def __init__(self, name):
        self.name = name
        self.other = None
    def __del__(self):
        print("del", self.name, file=sys.stderr)

a = Node("a")
b = Node("b")
a.other = b
b.other = a
c = Node("c")
c.self = c
del a, b, c
class Holder:
    def __del__(self):
        print("holder del", file=sys.stderr)
h = Holder()
h.me = h
print("end")
