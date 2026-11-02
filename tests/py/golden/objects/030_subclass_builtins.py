# Subclassing builtin types: list, dict (incl. __missing__), str, int, tuple; reprs, which
# methods return the subclass and which return the base type.


class MyList(list):
    def push(self, x):
        self.append(x)
        return self


ml = MyList([1, 2])
ml.push(3)
print("list subclass repr", ml, repr(ml), type(ml).__name__)
print("slice returns base", type(ml[:1]).__name__, type(ml + [4]).__name__, type(ml * 2).__name__)
print("isinstance list", isinstance(ml, list), ml == [1, 2, 3])
ml.attr = "has dict"
print("subclass has __dict__", ml.attr)


class DefaultDict(dict):
    def __init__(self, factory, *args):
        super().__init__(*args)
        self.factory = factory

    def __missing__(self, key):
        value = self[key] = self.factory()
        return value


dd = DefaultDict(list)
dd["a"].append(1)
dd["a"].append(2)
dd["b"]
print("missing", dd, type(dd).__name__)
print("get does not call missing", dd.get("zz"), "zz" in dd)


class Counter(dict):
    def __missing__(self, key):
        return 0


c = Counter()
for ch in "hello":
    c[ch] += 1
print("counter", c, c["nope"], len(c))


class NoMissing(dict):
    pass


try:
    NoMissing()["k"]
except KeyError as e:
    print(type(e).__name__, e)


class LoudDict(dict):
    def __setitem__(self, key, value):
        super().__setitem__(key, value * 10)


ld = LoudDict(a=1)
ld["b"] = 2
ld.update(c=3)
print("dict methods bypass __setitem__", ld)


class Upper(str):
    def shout(self):
        return self.upper() + "!"


u = Upper("hi")
print("str subclass", u, repr(u), u.shout(), type(u.upper()).__name__, type(u + "x").__name__)
print("str subclass eq hash", u == "hi", hash(u) == hash("hi"), {"hi": 1}[u])


class Tagged(str):
    def __new__(cls, value, tag):
        obj = super().__new__(cls, value)
        obj.tag = tag
        return obj


t = Tagged("text", "T")
print("str __new__", t, t.tag, len(t))


class Celsius(int):
    def __repr__(self):
        return "Celsius(" + int.__repr__(self) + ")"


deg = Celsius(21)
print("int subclass", deg, repr(deg), deg + 1, type(deg + 1).__name__, str(deg))
print("int subclass f-string", f"{deg}", f"{deg:03d}", "%d" % deg)


class Even(int):
    def __new__(cls, v):
        if v % 2:
            raise ValueError("odd: " + str(v))
        return super().__new__(cls, v)


print("int __new__", Even(4))
try:
    Even(3)
except ValueError as e:
    print(type(e).__name__, e)


class Pair(tuple):
    def __new__(cls, a, b):
        return super().__new__(cls, (a, b))

    @property
    def first(self):
        return self[0]


pr = Pair(1, 2)
print("tuple subclass", pr, pr.first, type(pr).__name__, pr == (1, 2))

for base in (bool, type(None), range, memoryview, slice):
    try:

        class Sub(base):
            pass

    except TypeError as e:
        print(type(e).__name__, e)


class F(float):
    pass


print("float subclass", F(1.5), F(1.5) * 2, type(F(1.5) * 2).__name__, repr(F("2.5")))


class MySet(set):
    pass


print("set subclass", sorted(MySet([3, 1])), type(MySet([1]) | {2}).__name__, MySet())
print("empty subclass reprs", MyList(), NoMissing(), Upper(), Celsius(), Pair(0, 0))
