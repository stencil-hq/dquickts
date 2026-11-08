# with statement: enter/exit order, as targets, suppression, multiple items, parenthesized form.

class CM:
    def __init__(self, name, suppress=False, value=None):
        self.name = name
        self.suppress = suppress
        self.value = value

    def __enter__(self):
        print("  enter", self.name)
        return self.value if self.value is not None else self

    def __exit__(self, exc_type, exc, tb):
        desc = "None" if exc_type is None else exc_type.__name__ + "(" + str(exc) + ")"
        print("  exit", self.name, desc, "tb set" if tb is not None else "tb None")
        return self.suppress

print("basic")
with CM("a") as cm:
    print("  body, as target is", cm.name)

print("as returns enter value")
with CM("b", value=42) as v:
    print("  value", v)

print("no as target")
with CM("c"):
    print("  body without as")

print("exception propagates")
try:
    with CM("d"):
        raise ValueError("inner")
except ValueError as e:
    print("  caught outside", type(e).__name__, e)

print("exception suppressed")
with CM("e", suppress=True):
    raise KeyError("swallowed")
    print("never")
print("  continued after suppressed")

print("multiple items: enter in order, exit reversed")
with CM("one") as a, CM("two") as b:
    print("  body", a.name, b.name)

print("parenthesized multiple items")
with (
    CM("p1") as p1,
    CM("p2", value="v2") as p2,
    CM("p3"),
):
    print("  body", p1.name, p2)

print("inner suppresses, outer sees nothing")
with CM("outer"), CM("inner", suppress=True):
    raise RuntimeError("x")
print("  after")

print("outer suppresses exception from inner exit chain")
with CM("outer2", suppress=True), CM("inner2"):
    raise RuntimeError("y")
print("  after")

print("return inside with still exits")
def ret():
    with CM("r"):
        return "returned"
print("  result", ret())

print("break/continue inside with")
for i in range(3):
    with CM("loop" + str(i)):
        if i == 0:
            continue
        if i == 1:
            break

print("exception in __enter__ skips __exit__")
class BadEnter:
    def __enter__(self):
        raise OSError("enter failed")

    def __exit__(self, *args):
        print("  never")

try:
    with CM("before"), BadEnter():
        print("  never")
except OSError as e:
    print("  caught", type(e).__name__, e)

print("exception in __exit__ replaces original")
class BadExit:
    def __enter__(self):
        return self

    def __exit__(self, *args):
        raise TypeError("exit failed")

try:
    with BadExit():
        raise ValueError("original")
except TypeError as e:
    print("  caught", type(e).__name__, e, "context:", type(e.__context__).__name__)

print("truthy non-bool from __exit__ suppresses")
class Truthy:
    def __enter__(self):
        return self

    def __exit__(self, *args):
        return "yes"

with Truthy():
    raise ZeroDivisionError
print("  suppressed by truthy string")

print("missing protocol")
for obj in [object(), 5]:
    try:
        with obj:
            pass
    except TypeError as e:
        print("  ", type(e).__name__, e)

print("as with tuple and attribute targets")
class Holder:
    pass

h = Holder()
with CM("t", value=(1, 2)) as (x, y):
    print("  unpacked", x, y)
with CM("u", value="attr") as h.slot:
    print("  attribute target", h.slot)

print("nested with")
with CM("n1"):
    with CM("n2"):
        print("  innermost")

print("context manager reuse")
reusable = CM("reuse")
for _ in range(2):
    with reusable:
        pass
