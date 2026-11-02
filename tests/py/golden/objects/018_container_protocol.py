# Container protocol on a user class: __len__, __getitem__, __setitem__, __delitem__, __iter__,
# __contains__, __bool__, __reversed__ and slice handling.


class Bag:
    def __init__(self, *items):
        self.items = list(items)
        self.log = []

    def __len__(self):
        return len(self.items)

    def __getitem__(self, key):
        self.log.append("get " + repr(key))
        if isinstance(key, slice):
            return Bag(*self.items[key])
        return self.items[key]

    def __setitem__(self, key, value):
        self.log.append("set " + repr(key))
        self.items[key] = value

    def __delitem__(self, key):
        self.log.append("del " + repr(key))
        del self.items[key]

    def __iter__(self):
        return iter(self.items)

    def __contains__(self, value):
        self.log.append("contains " + repr(value))
        return value in self.items

    def __reversed__(self):
        return iter(self.items[::-1])

    def __repr__(self):
        return "Bag" + repr(tuple(self.items))


b = Bag(1, 2, 3, 4)
print("len", len(b))
print("getitem", b[0], b[-1])
print("slice", b[1:3], b[::2], b[::-1])
print("slice with None", b[None:2])
b[0] = 10
del b[1]
print("after set del", b)
print("contains", 3 in b, 99 in b, 99 not in b)
print("iter", list(b), [x * 2 for x in b])
print("reversed", list(reversed(b)))
print("log", b.log)
try:
    b[10]
except IndexError as e:
    print(type(e).__name__, e)
print("bool from len", bool(Bag()), bool(Bag(0)))
print("unpack", *Bag("a", "b"))
x, *rest = Bag(1, 2, 3)
print("star unpack", x, rest)
print("sorted bag", sorted(Bag(3, 1, 2)), sum(Bag(1, 2, 3)), max(Bag(4, 9)))


class Flag:
    def __init__(self, on):
        self.on = on

    def __bool__(self):
        return self.on

    def __len__(self):
        return 0


print("bool beats len", bool(Flag(True)), bool(Flag(False)), "yes" if Flag(True) else "no")
print("not", not Flag(True), Flag(False) or "fallback", Flag(True) and "both")


class BadBool:
    def __bool__(self):
        return 1


try:
    bool(BadBool())
except TypeError as e:
    print(type(e).__name__, e)


class NegLen:
    def __len__(self):
        return -1


try:
    len(NegLen())
except ValueError as e:
    print(type(e).__name__, e)


class FloatLen:
    def __len__(self):
        return 1.5


try:
    len(FloatLen())
except TypeError as e:
    print(type(e).__name__, e)


class NoLen:
    pass


print("default truthy", bool(NoLen()))
try:
    len(NoLen())
except TypeError as e:
    print(type(e).__name__, e)
try:
    NoLen()[0]
except TypeError as e:
    print(type(e).__name__, e)
try:
    NoLen()[0] = 1
except TypeError as e:
    print(type(e).__name__, e)
try:
    del NoLen()[0]
except TypeError as e:
    print(type(e).__name__, e)
try:
    reversed(NoLen())
except TypeError as e:
    print(type(e).__name__, e)


class Grid:
    def __getitem__(self, key):
        return "key=" + repr(key)


g = Grid()
print("tuple key", g[1, 2], g[1:2, ::3], g[...], g[()])
