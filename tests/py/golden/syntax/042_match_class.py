# match statement: class patterns, __match_args__, builtin self-matching classes.

class Point:
    __match_args__ = ("x", "y")

    def __init__(self, x, y):
        self.x = x
        self.y = y

    def __repr__(self):
        return "Point(" + repr(self.x) + ", " + repr(self.y) + ")"

class Point3(Point):
    __match_args__ = ("x", "y", "z")

    def __init__(self, x, y, z):
        super().__init__(x, y)
        self.z = z

class NoArgs:
    def __init__(self, v):
        self.v = v

def where(p):
    match p:
        case Point(0, 0):
            return "origin"
        case Point3(x, y, z) if z != 0:
            return "3d " + repr((x, y, z))
        case Point(x=0, y=yy):
            return "on y axis at " + repr(yy)
        case Point(xx, 0):
            return "on x axis at " + repr(xx)
        case Point(x, y=y) if x == y:
            return "diagonal " + repr(x)
        case Point():
            return "somewhere " + repr(p)
        case NoArgs(v=val):
            return "noargs v=" + repr(val)
    return "not a point"

for p in [Point(0, 0), Point(0, 5), Point(3, 0), Point(2, 2), Point(1, 9), Point3(1, 2, 3),
          Point3(0, 0, 0), NoArgs(7), (0, 0), None]:
    print("where", repr(p) if not isinstance(p, NoArgs) else "NoArgs", where(p))

# positional subpatterns without __match_args__
match NoArgs(1):
    case NoArgs():
        print("no-arg class pattern ok")
try:
    match NoArgs(1):
        case NoArgs(1):
            pass
except TypeError as e:
    print("positional without __match_args__", type(e).__name__, e)

# too many positional subpatterns
try:
    match Point(1, 2):
        case Point(1, 2, 3):
            pass
except TypeError as e:
    print("too many positional", type(e).__name__, e)

# missing attribute just fails to match
match Point(1, 2):
    case Point(z=_):
        print("never")
    case _:
        print("missing attribute is no match")

# builtin types match their subject positionally (self)
def builtin(v):
    match v:
        case bool(b):
            return "bool " + repr(b)
        case int(i):
            return "int " + repr(i)
        case float(f):
            return "float " + repr(f)
        case str(s) if len(s) > 3:
            return "long str " + s
        case str():
            return "short str"
        case bytes(b):
            return "bytes " + repr(b)
        case list([first, *_]):
            return "list starting " + repr(first)
        case list():
            return "empty list"
        case tuple((a, b)):
            return "pair " + repr((a, b))
        case dict({"k": val}):
            return "dict with k=" + repr(val)
        case set() | frozenset():
            return "some set of size " + str(len(v))
    return "other " + type(v).__name__

for v in [True, 7, 2.5, "hello", "hi", b"x", [9, 8], [], (1, 2), (1, 2, 3), {"k": 0}, {"j": 0},
          {1}, frozenset(), None, 1j]:
    print("builtin", repr(v), builtin(v))

# class pattern matches subclasses (isinstance)
class MyInt(int):
    pass

print("subclass", builtin(MyInt(4)))

# class pattern requires a type
NotAType = "Point"
try:
    match Point(0, 0):
        case NotAType():
            pass
except TypeError as e:
    print("not a type", type(e).__name__, e)

# __match_args__ must be a tuple
class BadArgs:
    __match_args__ = ["a"]
    a = 1

try:
    match BadArgs():
        case BadArgs(1):
            pass
except TypeError as e:
    print("bad __match_args__", type(e).__name__, e)

# keyword and positional for the same attribute is an error
try:
    match Point(1, 2):
        case Point(1, x=1):
            pass
except TypeError as e:
    print("duplicate attr", type(e).__name__, e)

# nested class patterns
class Line:
    __match_args__ = ("start", "end")

    def __init__(self, start, end):
        self.start = start
        self.end = end

match Line(Point(0, 0), Point(3, 4)):
    case Line(Point(0, 0), Point(x2, y2) as e):
        print("nested class", x2, y2, e)
