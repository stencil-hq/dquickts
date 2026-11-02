# Legacy sequence iteration via __getitem__ only, membership fallbacks, and disabling
# protocols by setting them to None.


class Squares:
    def __init__(self, n):
        self.n = n
        self.requested = []

    def __getitem__(self, i):
        self.requested.append(i)
        if i >= self.n:
            raise IndexError(i)
        return i * i


sq = Squares(4)
print("iterate via getitem", list(sq))
print("indices requested", sq.requested)
print("for loop", [x for x in Squares(3)])
print("contains via iteration", 9 in Squares(5), 10 in Squares(5))
print("iter type", type(iter(Squares(1))).__name__)
print("unpack", *Squares(3))
a, b = Squares(2)
print("tuple unpack", a, b)
print("sum", sum(Squares(5)), "max", max(Squares(5)))
print("enumerate", list(enumerate(Squares(3))))
print("zip", list(zip(Squares(3), "abc")))
print("reversed needs len", end=" ")
try:
    reversed(Squares(3))
except TypeError as e:
    print(type(e).__name__, e)


class SquaresWithLen(Squares):
    def __len__(self):
        return self.n


print("reversed with len", list(reversed(SquaresWithLen(4))))


class StopsOnStop:
    def __getitem__(self, i):
        if i == 2:
            raise StopIteration
        return i


print("StopIteration also ends", list(StopsOnStop()))


class KeyErrorSeq:
    def __getitem__(self, i):
        raise KeyError(i)


try:
    list(KeyErrorSeq())
except KeyError as e:
    print("other errors propagate", type(e).__name__, e)


class NotIterable:
    __iter__ = None

    def __getitem__(self, i):
        return i


try:
    iter(NotIterable())
except TypeError as e:
    print(type(e).__name__, e)
print("still subscriptable", NotIterable()[7])


class NoContains:
    __contains__ = None

    def __iter__(self):
        return iter([1, 2])


try:
    1 in NoContains()
except TypeError as e:
    print(type(e).__name__, e)


class IterOnly:
    def __iter__(self):
        yield "x"
        yield "y"


print("contains via __iter__", "y" in IterOnly(), "z" in IterOnly())


class BadIter:
    def __iter__(self):
        return 5


try:
    iter(BadIter())
except TypeError as e:
    print(type(e).__name__, e)
try:
    for _ in BadIter():
        pass
except TypeError as e:
    print(type(e).__name__, e)


class NoNext:
    def __iter__(self):
        return self


try:
    iter(NoNext())
except TypeError as e:
    print(type(e).__name__, e)

seq = Squares(3)
it = iter(seq)
print("manual next", next(it), next(it), next(it))
try:
    next(it)
except StopIteration as e:
    print("exhausted", type(e).__name__, repr(e.args))
seq.n = 10
print("seq iterator stays exhausted", next(it, "done"))
print("string iteration", list("héllo"), list(iter("ab")))
print("dict iteration keys", list({"b": 1, "a": 2}))
