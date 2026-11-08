# Unary operators, __index__, __int__/__float__/__complex__, __matmul__, __round__ and friends.


class Num:
    def __init__(self, v):
        self.v = v

    def __repr__(self):
        return "Num(" + repr(self.v) + ")"

    def __neg__(self):
        return Num(-self.v)

    def __pos__(self):
        return Num(+self.v)

    def __abs__(self):
        return Num(abs(self.v))

    def __invert__(self):
        return Num(~self.v)

    def __int__(self):
        return int(self.v)

    def __float__(self):
        return float(self.v)

    def __complex__(self):
        return complex(self.v, 1)

    def __round__(self, ndigits=None):
        return "rounded " + repr(ndigits)

    def __trunc__(self):
        return "trunc"


n = Num(5)
print("unary", -n, +n, abs(Num(-3)), ~n)
print("conversions", int(n), float(n), complex(n))
print("round", round(n), round(n, 2))
for label, fn in [("neg str", lambda: -"s"), ("invert float", lambda: ~1.5), ("abs list", lambda: abs([]))]:
    try:
        fn()
    except TypeError as e:
        print(label, type(e).__name__, e)


class Idx:
    def __init__(self, i):
        self.i = i

    def __index__(self):
        return self.i


data = list(range(10))
print("index subscript", data[Idx(3)], data[Idx(-1)])
print("index slice", data[Idx(2) : Idx(5)], "abcdef"[Idx(1) :: Idx(2)])
print("index builtins", bin(Idx(5)), hex(Idx(255)), oct(Idx(8)))
print("index range", list(range(Idx(3))), "x" * Idx(3))
print("index int()", int(Idx(7)), [0] * Idx(2))
print("operator-free index", (5).__index__(), True.__index__())
try:
    data[1.0]
except TypeError as e:
    print(type(e).__name__, e)
try:
    data[Num(1)]
except TypeError as e:
    print(type(e).__name__, e)


class BadIndex:
    def __index__(self):
        return "nope"


try:
    data[BadIndex()]
except TypeError as e:
    print(type(e).__name__, e)


class BadInt:
    def __int__(self):
        return 1.5


try:
    int(BadInt())
except TypeError as e:
    print(type(e).__name__, e)


class Matrix:
    def __init__(self, rows):
        self.rows = rows

    def __matmul__(self, other):
        cols = list(zip(*other.rows))
        return Matrix([[sum(a * b for a, b in zip(r, c)) for c in cols] for r in self.rows])

    def __rmatmul__(self, other):
        return "rmatmul with " + type(other).__name__

    def __imatmul__(self, other):
        self.rows = (self @ other).rows
        return self

    def __repr__(self):
        return "Matrix(" + repr(self.rows) + ")"


m = Matrix([[1, 2], [3, 4]])
ident = Matrix([[1, 0], [0, 1]])
print("matmul", m @ m, m @ ident)
print("rmatmul", [1] @ m)
m @= m
print("imatmul", m)
try:
    1 @ 2
except TypeError as e:
    print(type(e).__name__, e)


class Bool:
    def __bool__(self):
        return False


print("not uses bool", not Bool(), (not Num(0)))
print("builtin unary", -(-5), +True, ~0, ~-1, -0.0, abs(-0.0), -(2**64))
print("int of float edge", int(-2.9), int(2.9), int(1e20))
for bad in ["inf", "nan"]:
    try:
        int(float(bad))
    except (OverflowError, ValueError) as e:
        print(type(e).__name__, e)
print("complex parts", complex(Num(2)).real, complex(Num(2)).imag)
print("float of Idx", float(Idx(4)))
