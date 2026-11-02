# add_note / __notes__, and the messages of common builtin runtime errors.

e = ValueError("base")
print("no notes attr", hasattr(e, "__notes__"))
e.add_note("first note")
e.add_note("second note")
print("notes", e.__notes__, type(e.__notes__).__name__)
print("str unaffected", str(e))
try:
    e.add_note(5)
except TypeError as err:
    print(type(err).__name__, err)
e.__notes__.append("appended directly")
print("notes list mutable", e.__notes__)


def annotate():
    try:
        int("bad")
    except ValueError as err:
        err.add_note("while parsing config")
        raise


try:
    annotate()
except ValueError as err:
    print("note survives re-raise", err, err.__notes__)


def check(label, func):
    try:
        result = func()
    except Exception as err:
        print(label, "->", type(err).__name__ + ":", err)
    else:
        print(label, "-> no error", repr(result))


undefined_global = None
del undefined_global

check("int division", lambda: 1 / 0)
check("int floordiv", lambda: 1 // 0)
check("int modulo", lambda: 1 % 0)
check("float division", lambda: 1.0 / 0)
check("float modulo", lambda: 1.5 % 0.0)
check("divmod", lambda: divmod(1, 0))
check("pow zero negative", lambda: 0**-1)
check("float pow zero negative", lambda: 0.0**-1)
check("dict key", lambda: {"a": 1}["b"])
check("dict int key", lambda: {}[0])
check("dict tuple key", lambda: {}[(1, 2)])
check("list index", lambda: [1, 2][5])
check("list assignment index", lambda: [].__setitem__(0, 1))
check("tuple index", lambda: (1,)[3])
check("str index", lambda: "ab"[9])
check("range index", lambda: range(3)[3])
check("pop empty list", lambda: [].pop())
check("pop empty dict", lambda: {}.popitem())
check("list remove", lambda: [1].remove(2))
check("list index method", lambda: [1].index(2))
check("str index method", lambda: "abc".index("z"))
check("int attribute", lambda: (1).x)
check("str attribute", lambda: "s".foo)
check("none call", lambda: None())
check("int call", lambda: 5())
check("add int str", lambda: 1 + "a")
check("add str int", lambda: "a" + 1)
check("sub str", lambda: "a" - "b")
check("mul list list", lambda: [1] * [2])
check("neg list", lambda: -[1])
check("compare", lambda: [] < 1)
check("name", lambda: undefined_global)
check("int literal", lambda: int("x"))
check("int literal base", lambda: int("9", 8))
check("int empty", lambda: int(""))
check("float literal", lambda: float("abc"))
check("int none", lambda: int(None))
check("iter int", lambda: iter(5))
check("next list", lambda: next([1]))
check("len int", lambda: len(5))
check("unpack too many", lambda: [(a, b) for a, b in [(1, 2, 3)]])
check("unpack too few", lambda: [(a, b, c) for a, b, c in [(1, 2)]])
check("unpack non-iterable", lambda: [a for a, b in [1]])
check("hash list", lambda: hash([]))
check("set of dicts", lambda: {frozenset(), {}} and None)
check("str join ints", lambda: ",".join([1, 2]))
check("chr range", lambda: chr(-1))
check("tuple assign", lambda: (1, 2).__setitem__(0, 5))
check("str assign", lambda: "abc".__setitem__(0, "x"))
check("missing method", lambda: [].push(1))
check("bad kwarg", lambda: sorted([], reversed=True))
check("int overflow float", lambda: float(10**400))
check("str mul negative", lambda: "ab" * -1)
check("range step zero", lambda: range(1, 2, 0))
check("slice step zero", lambda: [1, 2][::0])


def unbound():
    print(x)
    x = 1


check("unbound local", unbound)
