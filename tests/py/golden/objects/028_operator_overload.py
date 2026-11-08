# Binary operator overloading: __add__/__radd__/__iadd__, NotImplemented fallback to the
# reflected operation, unsupported operand TypeError messages, subclass priority.


class Vec:
    def __init__(self, *c):
        self.c = c

    def __repr__(self):
        return "Vec" + repr(self.c)

    def __add__(self, other):
        if isinstance(other, Vec):
            return Vec(*[a + b for a, b in zip(self.c, other.c)])
        if isinstance(other, (int, float)):
            return Vec(*[a + other for a in self.c])
        return NotImplemented

    def __radd__(self, other):
        if other == 0:
            return self
        return self.__add__(other)

    def __sub__(self, other):
        if not isinstance(other, Vec):
            return NotImplemented
        return Vec(*[a - b for a, b in zip(self.c, other.c)])

    def __mul__(self, k):
        if not isinstance(k, (int, float)):
            return NotImplemented
        return Vec(*[a * k for a in self.c])

    __rmul__ = __mul__

    def __pow__(self, k, mod=None):
        return Vec(*[pow(a, k, mod) for a in self.c])

    def __eq__(self, other):
        return isinstance(other, Vec) and self.c == other.c

    __hash__ = None


a, b = Vec(1, 2), Vec(10, 20)
print("add", a + b, a + 1, 1 + a)
print("sum uses radd", sum([a, b]), sum([a, b], Vec(0, 0)))
print("sub", b - a)
print("mul rmul", a * 3, 3 * a)
print("pow", a**2, pow(a, 3), pow(Vec(3, 4), 2, 5))
for label, fn in [
    ("vec + str", lambda: a + "s"),
    ("str + vec", lambda: "s" + a),
    ("vec - int", lambda: a - 1),
    ("vec * vec", lambda: a * b),
    ("vec @ vec", lambda: a @ b),
    ("vec << 1", lambda: a << 1),
    ("int + None", lambda: 1 + None),
    ("list + tuple", lambda: [1] + (2,)),
    ("str * float", lambda: "ab" * 2.0),
]:
    try:
        fn()
    except TypeError as e:
        print(label, type(e).__name__, e)


class Acc:
    def __init__(self):
        self.items = []

    def __iadd__(self, other):
        self.items.append(other)
        return self

    def __add__(self, other):
        return "plain add"


acc = Acc()
before = acc
acc += 1
acc += 2
print("iadd in place", acc.items, acc is before)


oa = Vec(1)
orig = oa
oa += 5
print("iadd falls back to add", oa, oa is orig)


class IaddNotImpl:
    def __iadd__(self, other):
        return NotImplemented

    def __add__(self, other):
        return "add after iadd NotImplemented"


x = IaddNotImpl()
x += 1
print("iadd NotImplemented", x)

lst = [1]
alias = lst
lst += (2, 3)
print("list iadd extends", lst, alias is lst)
t = (1,)
t_alias = t
t += (2,)
print("tuple iadd rebinds", t, t_alias)
try:
    lst = lst + (4,)
except TypeError as e:
    print(type(e).__name__, e)


class Base:
    def __add__(self, other):
        return "Base.__add__"

    def __radd__(self, other):
        return "Base.__radd__"


class Derived(Base):
    def __radd__(self, other):
        return "Derived.__radd__"


print("subclass reflected priority", Base() + Derived(), Derived() + Base())


class Same(Base):
    pass


print("subclass without override", Base() + Same())
print("divmod dunder", divmod(7, 2), (7).__divmod__(2), (7).__rdivmod__(30))
try:
    bool(NotImplemented)
except TypeError as e:
    print("NotImplemented truthiness", type(e).__name__, e)
