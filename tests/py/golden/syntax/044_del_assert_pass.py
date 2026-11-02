# del (names, items, slices, attributes), assert with messages, pass, Ellipsis bodies.

a = 1
del a
try:
    print(a)
except NameError as e:
    print("del name", type(e).__name__, e)

try:
    del never_defined
except NameError as e:
    print("del undefined", type(e).__name__, e)

x, y, z = 1, 2, 3
del x, y
print("del multiple, z remains", z)
try:
    x
except NameError as e:
    print("x gone", type(e).__name__, e)

# del in a function makes the name unbound local
def f():
    v = 1
    del v
    try:
        return v
    except UnboundLocalError as e:
        return type(e).__name__ + ": " + str(e)

print("del local", f())

# del items
d = {"a": 1, "b": 2, "c": 3}
del d["b"]
print("del dict item", d)
try:
    del d["zz"]
except KeyError as e:
    print("del missing key", type(e).__name__, e)

lst = [0, 1, 2, 3, 4, 5]
del lst[0]
del lst[-1]
del lst[1:3]
print("del list item/slice", lst)
del lst[:]
print("del all", lst)

# del attributes
class C:
    cls_attr = "class"

c = C()
c.inst = "instance"
del c.inst
try:
    c.inst
except AttributeError as e:
    print("del attr", type(e).__name__, e)
try:
    del c.cls_attr
except AttributeError as e:
    print("del class attr via instance", type(e).__name__, e)
del C.cls_attr
print("del class attr", hasattr(C, "cls_attr"))

# del tuple/list targets
p, q = [1, 2], {"k": 1}
del (p[0], q["k"])
print("del tuple target", p, q)
r1, r2 = 1, 2
del [r1, r2]
print("del list of names ok", "r1" in globals(), "r2" in globals())

# custom __delitem__ / __delattr__
class Tracker:
    def __delitem__(self, key):
        print("  __delitem__", repr(key))

    def __delattr__(self, name):
        print("  __delattr__", name)

t = Tracker()
del t[1], t["x":"y"]
del t.anything

# assert
assert True
assert 1 == 1, "never shown"
try:
    assert False
except AssertionError as e:
    print("assert no msg", type(e).__name__, repr(str(e)), e.args)

try:
    assert 1 > 2, "one is not greater than two"
except AssertionError as e:
    print("assert msg", type(e).__name__, e)

try:
    assert [], ["msg", "is", "a list"]
except AssertionError as e:
    print("assert non-str msg", e.args)

calls = []
def msg():
    calls.append("msg evaluated")
    return "lazy"

assert True, msg()
print("message lazy", calls)

try:
    assert (0, "tuple is truthy")
    print("tuple assertion passes (always true)")
except AssertionError:
    print("never")

# pass and Ellipsis as bodies
def empty():
    pass

def ellipsis_body():
    ...

class Empty:
    pass

print("pass returns None", empty(), ellipsis_body(), type(Empty()).__name__)
if True:
    pass
for _ in range(2):
    pass
while False:
    pass
print("pass statements ok")

# del of things that cannot be deleted (compile errors)
for src in ["del f()", "del 1", "del (a + b)", "del *a", "del a, *b", "del [a, (b, c)]"]:
    try:
        compile(src, "<d>", "exec")
        print("compiles", repr(src))
    except SyntaxError as e:
        print("syntax", repr(src), e.msg)
