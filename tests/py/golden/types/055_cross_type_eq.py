"""L0 golden: equality and ordering across different builtin types.

Different types are never equal unless numeric (int/float/complex/bool) or
str-vs-str-subclass etc.; ordering across unrelated types raises TypeError
with a message naming both types.

Usage: python3.15 -I -X utf8 tests/py/golden/types/055_cross_type_eq.py
"""


def show(label: str, thunk) -> None:
    """Print label and repr(thunk()), or the exception type and message."""
    try:
        print(label, repr(thunk()))
    except Exception as e:
        print(label, type(e).__name__, str(e))


SAMPLES = [0, 1, 1.0, True, 1j, "1", b"1", [1], (1,), {1}, frozenset({1}), {1: 1}, None, range(1, 2)]

for a in SAMPLES:
    row = "".join("=" if a == b else "." for b in SAMPLES)
    print("eq row", repr(a).ljust(16), row)

show("1 == '1'", lambda: 1 == "1")
show("[] == ()", lambda: [] == ())
show("[] == []", lambda: [] == [])
show("'' == b''", lambda: "" == b"")
show("b'' == bytearray()", lambda: b"" == bytearray())
show("set() == frozenset()", lambda: set() == frozenset())
show("{} == set()", lambda: {} == set())
show("range(0) == []", lambda: range(0) == [])
show("0 == None", lambda: 0 == None)
show("0 == ''", lambda: 0 == "")
show("0 == []", lambda: 0 == [])
show("1 is True", lambda: (1 == True, type(1) is type(True)))
show("2 == True", lambda: 2 == True)
show("'a' == 'a'", lambda: "a" == "a")
show("[1, 2] == [1, 2.0]", lambda: [1, 2] == [1, 2.0])
show("{'a': 1} == {'a': True}", lambda: {"a": 1} == {"a": True})
show("(1, [2]) == (1, [2])", lambda: (1, [2]) == (1, [2]))
show("dict views eq", lambda: {1: 2}.keys() == {1})

show("1 < '2'", lambda: 1 < "2")
show("'2' > 1", lambda: "2" > 1)
show("[] < ()", lambda: [] < ())
show("None < 0", lambda: None < 0)
show("None >= None", lambda: None >= None)
show("1 <= 1j", lambda: 1 <= 1j)
show("b'a' < 'a'", lambda: b"a" < "a")
show("{1} < [1, 2]", lambda: {1} < [1, 2])
show("{} < {}", lambda: {} < {})
show("1.5 < True", lambda: 1.5 < True)
show("False < 0.5 < True", lambda: False < 0.5 < True)
show("object() < object()", lambda: object() < object())
show("chained mixed", lambda: 1 < 2 < "3")
show("chained short-circuit", lambda: 2 < 1 < "3")

show("1 != '1'", lambda: 1 != "1")
show("None != 0", lambda: None != 0)
show("[] != ()", lambda: [] != ())
show("in mixed list", lambda: ("1" in [1, 2], 1 in ["1"], 1.0 in [1], True in [1]))
show("list.index mixed", lambda: [0, "", None, False].index(False))
show("dict key types distinct", lambda: len({1: 0, "1": 0, b"1": 0, (1,): 0, 1.5: 0}))
show("sorted mixed error", lambda: sorted([1, "1"]))
show("max mixed error", lambda: max([b"a", "a"]))
show("eq never raises", lambda: [x == y for x, y in ((1, "a"), ([], {}), (None, ...), (0j, 0))])
show("is vs ==", lambda: ([] is [], [] == []))
show("str subclass eq", lambda: type("S", (str,), {})("a") == "a")
show("int subclass eq", lambda: type("I", (int,), {})(5) == 5)
show("tuple subclass eq", lambda: type("T", (tuple,), {})((1,)) == (1,))
show("list vs list subclass", lambda: type("L", (list,), {})([1]) == [1])
