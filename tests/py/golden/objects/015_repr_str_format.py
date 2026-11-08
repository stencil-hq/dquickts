# __repr__, __str__, __format__ and how str()/repr()/print/format/f-strings use them.


class Point:
    def __init__(self, x, y):
        self.x = x
        self.y = y

    def __repr__(self):
        return "Point(" + repr(self.x) + ", " + repr(self.y) + ")"

    def __str__(self):
        return "(" + str(self.x) + ", " + str(self.y) + ")"

    def __format__(self, spec):
        if spec == "":
            return str(self)
        if spec == "polar-ish":
            return "r2=" + str(self.x**2 + self.y**2)
        return "(" + format(self.x, spec) + ", " + format(self.y, spec) + ")"


p = Point(1, 2.5)
print("print uses str", p)
print("str", str(p))
print("repr", repr(p))
print("in list uses repr", [p, p])
print("in dict uses repr", {"k": p})
print("format empty", format(p))
print("format spec", format(p, ".2f"))
print("format custom", format(p, "polar-ish"))
print("fstring", f"{p}", f"{p!r}", f"{p!s}", f"{p:08.3f}")
print("str.format", "{0} {0!r} {1:>6}".format(p, "x"))
print("percent", "%s | %r" % (p, p))


class OnlyRepr:
    def __repr__(self):
        return "OnlyRepr()"


o = OnlyRepr()
print("str falls back to repr", str(o), o, f"{o}")


class OnlyStr:
    def __str__(self):
        return "only str"


s = OnlyStr()
print("str only", str(s), repr(s).startswith("<__main__.OnlyStr object at "))
print("format falls back to str", format(s), f"{s}")
try:
    format(s, "10")
except TypeError as e:
    print(type(e).__name__, e)


class BadRepr:
    def __repr__(self):
        return 42


try:
    repr(BadRepr())
except TypeError as e:
    print(type(e).__name__, e)


class BadStr:
    def __str__(self):
        return None


try:
    str(BadStr())
except TypeError as e:
    print(type(e).__name__, e)


class BadFormat:
    def __format__(self, spec):
        return 1


try:
    format(BadFormat(), "")
except TypeError as e:
    print(type(e).__name__, e)


class Unicode:
    def __repr__(self):
        return "Ünï(é)"


print("non-ascii repr", repr(Unicode()), ascii(Unicode()))


class StrSub(str):
    pass


class ReturnsSub:
    def __str__(self):
        return StrSub("sub")


r = str(ReturnsSub())
print("str returns subclass", r, type(r).__name__)


class Money:
    def __init__(self, cents):
        self.cents = cents

    def __format__(self, spec):
        amount = str(self.cents // 100) + "." + str(self.cents % 100).zfill(2)
        return format(amount, spec) if spec else amount

    def __repr__(self):
        return "Money(" + str(self.cents) + ")"


m = Money(12345)
print("money", f"[{m}]", f"[{m:>10}]", f"[{m:*<10}]", f"{m!r}")
print("nested format spec", f"[{m:>{12}}]")
print("builtin formats", format(255, "x"), format(3.14159, ".2f"), format("ab", "^6"), format(1234567, ","))
print("format of int subclass", format(True, "d"), format(True), repr(True))


class ReprCalls:
    calls = 0

    def __repr__(self):
        ReprCalls.calls += 1
        return "R"


rc = ReprCalls()
print("repr of tuple", (rc, rc), ReprCalls.calls)
print("type repr", repr(Point), str(Point), repr(int))
