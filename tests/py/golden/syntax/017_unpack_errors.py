# Unpacking errors: too many/not enough values, non-iterables, partial assignment.

def attempt(label, fn):
    try:
        print(label, "ok", fn())
    except Exception as e:
        print(label, type(e).__name__, e)

def two(seq):
    a, b = seq
    return a, b

def three(seq):
    a, b, c = seq
    return a, b, c

def starred(seq):
    a, *b, c = seq
    return a, b, c

def nested(seq):
    a, (b, c) = seq
    return a, b, c

attempt("two from 3", lambda: two([1, 2, 3]))
attempt("two from 1", lambda: two([1]))
attempt("two from 0", lambda: two([]))
attempt("two from str 3", lambda: two("abc"))
attempt("two from str 1", lambda: two("a"))
attempt("two from int", lambda: two(5))
attempt("two from None", lambda: two(None))
attempt("two from float", lambda: two(1.5))
attempt("two from dict 3", lambda: two({1: 1, 2: 2, 3: 3}))
attempt("two from set 1", lambda: two({1}))
attempt("two from gen 3", lambda: two(x for x in range(3)))
attempt("two from gen 1", lambda: two(x for x in range(1)))
attempt("two from range 5", lambda: two(range(5)))
attempt("three from 2", lambda: three((1, 2)))
attempt("three from 5", lambda: three((1, 2, 3, 4, 5)))
attempt("three from big list", lambda: three(list(range(100))))
attempt("starred from 1", lambda: starred([1]))
attempt("starred from 0", lambda: starred([]))
attempt("starred from int", lambda: starred(3))
attempt("starred ok", lambda: starred([1, 2]))
attempt("nested inner short", lambda: nested((1, (2,))))
attempt("nested inner long", lambda: nested((1, (2, 3, 4))))
attempt("nested inner non-iterable", lambda: nested((1, 2)))
attempt("nested ok", lambda: nested((1, "bc")))

def single(seq):
    (a,) = seq
    return a

attempt("single from 2", lambda: single([1, 2]))
attempt("single from 0", lambda: single(()))

# iterator with __len__-less custom iterable
class Three:
    def __iter__(self):
        return iter("xyz")

attempt("custom iterable too many", lambda: two(Three()))
attempt("custom iterable ok", lambda: three(Three()))

# iterator that raises midway
def boom():
    yield 1
    raise RuntimeError("generator failed")

attempt("generator raises", lambda: two(boom()))

# a length mismatch is detected before any target is assigned
x = y = "unset"
try:
    x, y = [1, 2, 3]
except ValueError as e:
    print("after failed unpack", type(e).__name__, x, y)

# but subscript targets of a successful unpack are assigned in order and can fail midway
lst = [0]
try:
    lst[0], lst[5] = "a", "b"
except IndexError as e:
    print("midway target failure", type(e).__name__, e, lst)

# unpacking into too many starred targets is a SyntaxError (compile-time)
for src in ["a, *b, *c = [1, 2, 3]", "*a = [1]", "a, b = *c", "del *a"]:
    try:
        compile(src, "<test>", "exec")
        print("compiled", repr(src))
    except SyntaxError as e:
        print("syntax", repr(src), type(e).__name__, e.msg)

# unpack in for with wrong shapes
pairs = [(1, 2), (3,), (4, 5)]
done = []
try:
    for a, b in pairs:
        done.append(a + b)
except ValueError as e:
    print("loop unpack fail", done, type(e).__name__, e)

# unpack of infinite iterator into fixed targets stops early with error
def count():
    n = 0
    while True:
        yield n
        n += 1

attempt("infinite into two", lambda: two(count()))
