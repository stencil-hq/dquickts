"""L0 golden: numeric equality and hashing across int, float, bool, complex.

Equal numbers must hash equal so they merge as dict keys and set members; the
first inserted key object is kept while the value is overwritten.  Numeric
hashes are deterministic (unlike str hashes) so they are printed directly.

Usage: python3.15 -I -X utf8 tests/py/golden/types/021_numeric_hash_eq.py
"""


def show(label: str, thunk) -> None:
    """Print label and repr(thunk()), or the exception type and message."""
    try:
        print(label, repr(thunk()))
    except Exception as e:
        print(label, type(e).__name__, str(e))


show("1 == 1.0 == True", lambda: 1 == 1.0 == True)
show("1 == 1+0j", lambda: 1 == 1 + 0j)
show("0 == 0.0 == False == -0.0 == 0j", lambda: 0 == 0.0 == False == -0.0 == 0j)
show("dict merge", lambda: {1: "a", 1.0: "b", True: "c"})
show("dict merge float first", lambda: {1.0: "a", 1: "b"})
show("dict merge complex", lambda: {1j * 0 + 2: "x", 2: "y", 2.0: "z"})
show("set merge", lambda: {1, 1.0, True})
show("set merge float first", lambda: {1.0, 1, True})
show("set zeros", lambda: {0.0, 0, False, -0.0})
show("len set mixed", lambda: len({1, 1.0, True, 2, 2.0, 0, False, 0j}))
d = {1: "int"}
d[1.0] = "float"
d[True] = "bool"
show("keys kept", lambda: list(d))
show("lookup by float", lambda: d[1.0])
show("lookup by complex", lambda: d[1 + 0j])
show("'1' in d", lambda: "1" in d)

for v in (0, 1, -1, -2, 2, 7, -7, 2 ** 31, 2 ** 61 - 2, 2 ** 61 - 1, 2 ** 61, 2 ** 62, -(2 ** 61) + 1,
          -(2 ** 61), 10 ** 30, -(10 ** 30), True, False):
    print("hash int", v, hash(v))

show("hash(-1)", lambda: hash(-1))
show("hash(-2)", lambda: hash(-2))
show("hash(-1) == hash(-2)", lambda: hash(-1) == hash(-2))
show("hash(1) == hash(1.0)", lambda: hash(1) == hash(1.0))
show("hash(1) == hash(True)", lambda: hash(1) == hash(True))
show("hash(1) == hash(1+0j)", lambda: hash(1) == hash(1 + 0j))
show("hash(2**61-1)", lambda: hash(2 ** 61 - 1))

for f in (0.0, -0.0, 0.5, 1.5, -1.5, 0.1, 1e100, 2.0 ** 61, 2.0 ** 70, 5e-324, -1.0, 1e300):
    print("hash float", repr(f), hash(f))

show("hash(inf)", lambda: hash(float("inf")))
show("hash(-inf)", lambda: hash(float("-inf")))
show("hash(1e100) == hash(int(1e100))", lambda: hash(1e100) == hash(int(1e100)))
show("hash(2.0**70) == hash(2**70)", lambda: hash(2.0 ** 70) == hash(2 ** 70))
show("hash(1j)", lambda: hash(1j))
show("hash(1+2j)", lambda: hash(1 + 2j))
show("hash(-1+0j)", lambda: hash(-1 + 0j))
show("hash(()) ", lambda: hash(()))
show("hash((1, 2))", lambda: hash((1, 2)))
show("hash((1, 2)) == hash((1.0, 2.0))", lambda: hash((1, 2)) == hash((1.0, 2.0)))
show("hash(None) type", lambda: type(hash(None)).__name__)
show("hash(frozenset()) ", lambda: hash(frozenset()))
show("hash(frozenset({1, 2}))", lambda: hash(frozenset({1, 2})))
show("hash(range(3)) == hash(range(3))", lambda: hash(range(3)) == hash(range(3)))
show("hash([])", lambda: hash([]))
show("hash({})", lambda: hash({}))
show("hash(set())", lambda: hash(set()))
show("hash(bytearray())", lambda: hash(bytearray()))
show("hash(slice(1,2))", lambda: hash(slice(1, 2)) == hash(slice(1, 2)))
show("hash((1, [2]))", lambda: hash((1, [2])))

show("0.5 == 1/2", lambda: 0.5 == 1 / 2)
show("0.1 + 0.2 == 0.3", lambda: 0.1 + 0.2 == 0.3)
show("1 == '1'", lambda: 1 == "1")
show("1 != '1'", lambda: 1 != "1")
show("1 == [1]", lambda: 1 == [1])
show("1 == None", lambda: 1 == None)
show("1.0 is not 1", lambda: type(1.0) is not type(1))
show("[1, 2] == [1.0, 2.0]", lambda: [1, 2] == [1.0, 2.0])
show("(1, 2) == (True, 2.0)", lambda: (1, 2) == (True, 2.0))
show("{1: 2} == {1.0: 2.0}", lambda: {1: 2} == {1.0: 2.0})
show("{1} == {1.0}", lambda: {1} == {1.0})
show("[0.0] == [-0.0]", lambda: [0.0] == [-0.0])
show("big == float", lambda: 2 ** 100 == 2.0 ** 100)
show("big+1 != float", lambda: 2 ** 100 + 1 != 2.0 ** 100)
show("hash big+1 differs", lambda: hash(2 ** 100 + 1) != hash(2.0 ** 100))
show("1 in [1.0]", lambda: 1 in [1.0])
show("True in {1: 0}", lambda: True in {1: 0})
show("[1.0, True].index(1)", lambda: [1.0, True].index(1))
show("[1, 1.0, True].count(True)", lambda: [1, 1.0, True].count(True))
