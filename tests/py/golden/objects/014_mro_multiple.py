# Multiple inheritance: diamond MRO, cooperative super(), C3 linearization failures.


class A:
    def who(self):
        return ["A"]


class B(A):
    def who(self):
        return ["B"] + super().who()


class C(A):
    def who(self):
        return ["C"] + super().who()


class D(B, C):
    def who(self):
        return ["D"] + super().who()


print("diamond mro", [c.__name__ for c in D.__mro__])
print("mro()", [c.__name__ for c in D.mro()])
print("cooperative", D().who())
print("object last", D.__mro__[-1] is object)


class E(C, B):
    pass


print("reversed bases", [c.__name__ for c in E.__mro__])

try:

    class Bad(D, E):
        pass

except TypeError as e:
    print(type(e).__name__, e)

try:

    class Bad2(A, B):
        pass

except TypeError as e:
    print(type(e).__name__, e)

try:

    class Dup(A, A):
        pass

except TypeError as e:
    print(type(e).__name__, e)


class Base:
    def __init__(self, **kw):
        self.trail = ["Base"]
        if kw:
            self.leftover = kw


class Left(Base):
    def __init__(self, left=None, **kw):
        super().__init__(**kw)
        self.trail.append("Left=" + str(left))


class Right(Base):
    def __init__(self, right=None, **kw):
        super().__init__(**kw)
        self.trail.append("Right=" + str(right))


class Both(Left, Right):
    def __init__(self, **kw):
        super().__init__(**kw)
        self.trail.append("Both")


both = Both(left=1, right=2)
print("cooperative init", both.trail)
print("leftover", hasattr(both, "leftover"))
print("leftover kw", Both(left=1, extra=3).leftover)


class X:
    pass


class Y:
    pass


class Z:
    pass


class K1(X, Y):
    pass


class K2(Y, Z):
    pass


class K3(K1, K2, Z):
    pass


print("wikipedia-style mro", [c.__name__ for c in K3.__mro__])


class Attr1:
    value = 1


class Attr2:
    value = 2


class Pick(Attr1, Attr2):
    pass


class Pick2(Attr2, Attr1):
    pass


print("attr resolution", Pick.value, Pick2.value)
print("bases", [b.__name__ for b in Pick.__bases__])
print("isinstance multi", isinstance(D(), C), isinstance(D(), B), issubclass(D, (int, A)))


print("super skip", super(B, D()).who())

try:

    class FromInt(int, str):
        pass

except TypeError as e:
    print(type(e).__name__, e)
