"""L0 golden: collections.deque, OrderedDict, namedtuple, ChainMap."""
from collections import ChainMap, OrderedDict, deque, namedtuple

# deque
d = deque([1, 2, 3])
d.append(4)
d.appendleft(0)
print("deque", d)
print("pop", d.pop(), d.popleft(), d)
d.extend([5, 6])
d.extendleft([-1, -2])
print("extend", d)
d.rotate(2)
print("rotate 2", d)
d.rotate(-3)
print("rotate -3", d)
print("index/count", d.index(5), d.count(1), d[0], d[-1])
d.remove(5)
d.reverse()
print("remove/reverse", d, len(d))
d.insert(1, 99)
print("insert", d)
del d[0]
print("del", d)
print("contains", 99 in d, 1000 in d)

m = deque(maxlen=3)
for i in range(6):
    m.append(i)
print("maxlen", m, m.maxlen)
m.appendleft(-1)
print("maxlen left", m)
m.extend("ab")
print("maxlen extend", m)
print("maxlen 0", deque([1, 2], maxlen=0))
try:
    deque().pop()
except IndexError as e:
    print("empty pop", type(e).__name__, e)
try:
    deque([1]).popleft()
    deque().popleft()
except IndexError as e:
    print("empty popleft", type(e).__name__, e)
print("eq", deque([1, 2]) == deque([1, 2]), deque([1]) == [1])
print("add", deque([1]) + deque([2]), deque([1, 2]) * 2)
print("copy", deque("xy", 5).copy())

# OrderedDict
od = OrderedDict(a=1, b=2, c=3)
od.move_to_end("a")
print("move_to_end", list(od))
od.move_to_end("c", last=False)
print("move_to_end first", list(od))
print("popitem", od.popitem(), list(od))
print("popitem first", od.popitem(last=False), list(od))
print("repr", OrderedDict([("x", 1), ("y", 2)]))
o1 = OrderedDict([("a", 1), ("b", 2)])
o2 = OrderedDict([("b", 2), ("a", 1)])
print("od eq order-sensitive", o1 == o2)
print("od eq dict", o1 == {"b": 2, "a": 1}, dict(o1) == dict(o2))
print("reversed", list(reversed(o1)))
try:
    OrderedDict().popitem()
except KeyError as e:
    print("empty popitem", type(e).__name__, e)
try:
    o1.move_to_end("zz")
except KeyError as e:
    print("move missing", type(e).__name__, e)

# namedtuple
Point = namedtuple("Point", ["x", "y"])
p = Point(1, y=2)
print("nt", p, p.x, p[1], tuple(p))
print("nt _fields", Point._fields)
print("nt _asdict", p._asdict())
print("nt _replace", p._replace(x=10))
print("nt _make", Point._make([5, 6]))
print("nt eq tuple", p == (1, 2), p < Point(1, 3))
print("nt unpack", *p)
Emp = namedtuple("Emp", "name dept salary", defaults=["eng", 100])
print("nt defaults", Emp("ann"), Emp._field_defaults)
print("nt rename", namedtuple("R", ["a", "def", "a", "_x"], rename=True)._fields)
try:
    Point(1)
except TypeError as e:
    print("nt missing", type(e).__name__, e)
try:
    p.x = 5
except AttributeError as e:
    print("nt immutable", type(e).__name__, e)
try:
    namedtuple("Bad", ["class"])
except ValueError as e:
    print("nt keyword", type(e).__name__, e)
try:
    p._replace(z=1)
except (ValueError, TypeError) as e:
    print("nt replace bad", type(e).__name__, e)


class Point3(namedtuple("Point3", "x y z")):
    __slots__ = ()

    def norm1(self):
        return abs(self.x) + abs(self.y) + abs(self.z)


print("nt subclass", Point3(1, -2, 3), Point3(1, -2, 3).norm1())

# ChainMap
defaults = {"color": "red", "user": "guest"}
args = {"user": "admin"}
cm = ChainMap(args, defaults)
print("chain lookup", cm["user"], cm["color"], len(cm), sorted(cm))
print("chain repr", cm)
cm["color"] = "blue"
print("chain write first", args, defaults)
child = cm.new_child({"user": "child"})
print("new_child", child["user"], child.parents["user"])
print("maps", len(child.maps), child.maps[0])
del cm["color"]
print("after del", cm["color"])
try:
    del cm["missing"]
except KeyError as e:
    print("del missing", type(e).__name__, e)
print("chain dict", dict(ChainMap({"a": 1}, {"a": 2, "b": 3})))
