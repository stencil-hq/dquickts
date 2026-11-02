# Truthiness of user-defined objects: __bool__, __len__, precedence, errors.

class Plain:
    pass

class WithBool:
    def __init__(self, v):
        self.v = v

    def __bool__(self):
        return self.v

class WithLen:
    def __init__(self, n):
        self.n = n

    def __len__(self):
        return self.n

class Both:
    def __bool__(self):
        return True

    def __len__(self):
        return 0

print("plain object", bool(Plain()))
print("bool True", bool(WithBool(True)))
print("bool False", bool(WithBool(False)))
print("len 0", bool(WithLen(0)))
print("len 3", bool(WithLen(3)))
print("bool wins over len", bool(Both()))
print("not len 0", not WithLen(0))
print("if len 3", "yes" if WithLen(3) else "no")

def check(label, obj):
    try:
        print(label, bool(obj))
    except Exception as e:
        print(label, type(e).__name__, e)

class BoolReturnsInt:
    def __bool__(self):
        return 0

class BoolReturnsNone:
    def __bool__(self):
        return None

class LenNegative:
    def __len__(self):
        return -1

class LenFloat:
    def __len__(self):
        return 1.0

class LenHuge:
    def __len__(self):
        return 2**70

class LenBool:
    def __len__(self):
        return True

check("bool returns int", BoolReturnsInt())
check("bool returns None", BoolReturnsNone())
check("len negative", LenNegative())
check("len float", LenFloat())
check("len huge", LenHuge())
check("len bool", LenBool())

# __bool__ = None makes the object unusable in boolean context
class BoolNone:
    __bool__ = None

check("__bool__ None", BoolNone())

# __len__ set on the instance is ignored (special lookup on type)
p = Plain()
p.__len__ = lambda: 0
p.__bool__ = lambda: False
print("instance attrs ignored", bool(p))

# inheritance of __bool__ and __len__
class Child(WithLen):
    pass

print("inherited len", bool(Child(0)), bool(Child(1)))

class Override(WithBool):
    def __bool__(self):
        return not self.v

print("overridden bool", bool(Override(True)), bool(Override(False)))

# subclasses of builtins keep builtin truthiness
class MyList(list):
    pass

class MyStr(str):
    pass

class MyInt(int):
    pass

print("list subclass", bool(MyList()), bool(MyList([1])))
print("str subclass", bool(MyStr("")), bool(MyStr("x")))
print("int subclass", bool(MyInt(0)), bool(MyInt(7)))

class ZeroLenList(list):
    def __len__(self):
        return 0

print("list subclass len override", bool(ZeroLenList([1, 2])))

class FalseInt(int):
    def __bool__(self):
        return False

print("int subclass bool override", bool(FalseInt(5)), FalseInt(5) + 1)

# __bool__ call count in and/or/not/if
calls = []
class Counted:
    def __init__(self, name, v):
        self.name, self.v = name, v

    def __bool__(self):
        calls.append(self.name)
        return self.v

a, b = Counted("a", False), Counted("b", True)
if a or b:
    pass
print("calls in if-or", calls)
calls.clear()
r = a or b
print("calls in expr-or", calls, r is b)
calls.clear()
r = not (a and b)
print("calls in not-and", calls, r)
calls.clear()
while b and not a:
    break
print("calls in while", calls)
