"""L0 golden: str/repr of large ints and exact int/float comparisons.

Int-to-float comparisons in Python are exact (never via lossy conversion),
including big ints beyond the float range and the 2**53 precision boundary.
The str() digit limit (4300 digits) applies to decimal conversion.

Usage: python3.15 -I -X utf8 tests/py/golden/types/013_int_str_compare.py
"""


def show(label: str, thunk) -> None:
    """Print label and repr(thunk()), or the exception type and message."""
    try:
        print(label, repr(thunk()))
    except Exception as e:
        print(label, type(e).__name__, str(e))


show("str(10**50)", lambda: str(10 ** 50))
show("str(-10**50 + 1)", lambda: str(-10 ** 50 + 1))
show("repr(2**127 - 1)", lambda: repr(2 ** 127 - 1))
show("str(-(2**63))", lambda: str(-(2 ** 63)))
show("len(str(2**10000))", lambda: len(str(2 ** 10000)))
show("str(2**10000)[:20]", lambda: str(2 ** 10000)[:20])
show("str(2**10000)[-20:]", lambda: str(2 ** 10000)[-20:])
show("str(10**4299) len", lambda: len(str(10 ** 4299)))
show("str(10**4300)", lambda: str(10 ** 4300))
show("hex(10**5000) len", lambda: len(hex(10 ** 5000)))
show("f'{10**30}'", lambda: f"{10 ** 30}")
show("'%d' % 10**25", lambda: "%d" % 10 ** 25)
show("str roundtrip", lambda: all(int(str(v)) == v for v in (3 ** 300, -7 ** 200, 10 ** 99 - 1)))

big = 2 ** 53
show("2**53 == 2.0**53", lambda: big == 2.0 ** 53)
show("2**53+1 == 2.0**53", lambda: big + 1 == 2.0 ** 53)
show("2**53+1 > 2.0**53", lambda: big + 1 > 2.0 ** 53)
show("2.0**53 < 2**53+1", lambda: 2.0 ** 53 < big + 1)
show("2**53+1 == float(2**53+1)", lambda: big + 1 == float(big + 1))
show("float(2**53+1)", lambda: float(big + 1))
show("float(2**53+3)", lambda: float(big + 3))
show("2**1024 > 1e308", lambda: 2 ** 1024 > 1e308)
show("2**1024 < inf", lambda: 2 ** 1024 < float("inf"))
show("-2**1024 > -inf", lambda: -2 ** 1024 > float("-inf"))
show("10**400 == inf", lambda: 10 ** 400 == float("inf"))
show("10**400 > 1.7976931348623157e308", lambda: 10 ** 400 > 1.7976931348623157e308)
show("1 == nan", lambda: 1 == float("nan"))
show("1 < nan", lambda: 1 < float("nan"))
show("1 != nan", lambda: 1 != float("nan"))
show("10**400 < nan", lambda: 10 ** 400 < float("nan"))
show("0 == -0.0", lambda: 0 == -0.0)
show("1 < 1.5 < 2", lambda: 1 < 1.5 < 2)
show("-1 > -1.5", lambda: -1 > -1.5)
show("10**16 == 1e16", lambda: 10 ** 16 == 1e16)
show("10**23 == 1e23", lambda: 10 ** 23 == 1e23)
show("10**22 == 1e22", lambda: 10 ** 22 == 1e22)
show("int(1e23)", lambda: int(1e23))
show("10**23 > 1e23", lambda: 10 ** 23 > 1e23)
show("2**63 == 9.223372036854776e18", lambda: 2 ** 63 == 9.223372036854776e18)
show("2**63-1 < 9.223372036854776e18", lambda: 2 ** 63 - 1 < 9.223372036854776e18)
show("2**63-1 == float(2**63-1)", lambda: 2 ** 63 - 1 == float(2 ** 63 - 1))
show("max(2**53+1, 2.0**53)", lambda: max(big + 1, 2.0 ** 53))
show("min(2**53, 2.0**53)", lambda: min(2.0 ** 53, big))
show("sorted mixed", lambda: sorted([3, 2.5, -1, 10 ** 20, 1e19, -1e20, 0, -0.0, True]))
show("float(10**308)", lambda: float(10 ** 308))
show("float(10**309)", lambda: float(10 ** 309))
show("float(2**1024)", lambda: float(2 ** 1024))
show("float(2**1024 - 2**970)", lambda: float(2 ** 1024 - 2 ** 970))
show("float(2**1024 - 2**971)", lambda: float(2 ** 1024 - 2 ** 971))
show("10**400 + 0.5", lambda: 10 ** 400 + 0.5)
show("10**400 * 1.0", lambda: 10 ** 400 * 1.0)
show("10**400 / 10**390", lambda: 10 ** 400 / 10 ** 390)
show("int == float hash", lambda: hash(2 ** 60) == hash(2.0 ** 60))
show("1 == 1.0000000000000002", lambda: 1 == 1.0000000000000002)
show("int < str", lambda: 1 < "2")
show("int == str", lambda: 1 == "1")
show("int == None", lambda: 0 == None)
show("int < None", lambda: 0 < None)
