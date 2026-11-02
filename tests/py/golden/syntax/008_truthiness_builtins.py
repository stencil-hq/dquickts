# Truthiness of builtin types: empty vs non-empty, zero vs non-zero.

def truth(label, value):
    print(label, repr(value), bool(value), "truthy" if value else "falsy")

truth("None", None)
truth("True", True)
truth("False", False)
truth("int 0", 0)
truth("int 1", 1)
truth("int -1", -1)
truth("big int", 10**100)
truth("float 0.0", 0.0)
truth("float -0.0", -0.0)
truth("float tiny", 5e-324)
truth("float inf", float("inf"))
truth("float nan", float("nan"))
truth("complex 0j", 0j)
truth("complex -0j", complex(-0.0, -0.0))
truth("complex 1j", 1j)
truth("complex real", complex(1, 0))
truth("str empty", "")
truth("str space", " ")
truth("str zero", "0")
truth("str False", "False")
truth("str nul", "\0")
truth("bytes empty", b"")
truth("bytes nul", b"\x00")
truth("bytearray empty", bytearray())
truth("bytearray one", bytearray(1))
truth("list empty", [])
truth("list [None]", [None])
truth("list [[]]", [[]])
truth("tuple empty", ())
truth("tuple (0,)", (0,))
truth("dict empty", {})
truth("dict nonempty", {None: None})
truth("set empty", set())
truth("set {0}", {0})
truth("frozenset empty", frozenset())
truth("frozenset nonempty", frozenset([0]))
truth("range 0", range(0))
truth("range 1", range(1))
truth("range empty neg", range(5, 0))
truth("range neg step", range(5, 0, -1))
truth("Ellipsis", ...)
print("memoryview empty", bool(memoryview(b"")), "nonempty", bool(memoryview(b"x")))
truth("slice", slice(0, 0))
truth("type", int)

# bool() of functions and modules-free objects
def fn():
    pass

print("function truthy", bool(fn))
print("lambda truthy", bool(lambda: 0))
print("builtin truthy", bool(len))
print("object() truthy", bool(object()))
print("class truthy", bool(type("X", (), {})))

# bool is a subclass of int
print("True + True", True + True)
print("True * 3", True * 3)
print("False - 1", False - 1)
print("isinstance bool int", isinstance(True, int))
print("bool(int subclass)", bool(True), int(True), int(False))
print("True == 1", True == 1, "False == 0", False == 0)
print("hash eq", hash(True) == hash(1))
print("dict key merge", {1: "int", True: "bool"})
print("str(True)", str(True), "repr(False)", repr(False))
print("bool('False')", bool("False"))
print("bool()", bool())

# not on NotImplemented is an error
try:
    print("not NotImplemented", not NotImplemented)
except TypeError as e:
    print("not NotImplemented", type(e).__name__, e)

# bool() rejects keyword arguments
try:
    bool(x=1)
except TypeError as e:
    print("bool kw", type(e).__name__, e)

# all/any rely on truthiness
print("all empty", all([]))
print("any empty", any([]))
print("all mixed", all([1, "a", [0]]), all([1, "", 2]))
print("any mixed", any([0, "", None, [], 0.0]), any([0, "", (0,)]))

# filter(None, ...) uses truthiness
vals = [0, 1, "", "a", None, [], [0], 0.0, 2.5, (), {}, {1}]
print("filter None", list(filter(None, vals)))
print("count truthy", sum(1 for v in vals if v))
