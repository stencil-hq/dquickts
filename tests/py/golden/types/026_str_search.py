"""L0 golden: str searching: startswith/endswith, find/rfind, index/rindex, count, in.

Covers tuple prefixes, start/end arguments (including negative and out of
range), empty substrings, non-overlapping count, and ValueError messages.

Usage: python3.15 -I -X utf8 tests/py/golden/types/026_str_search.py
"""


def show(label: str, thunk) -> None:
    """Print label and repr(thunk()), or the exception type and message."""
    try:
        print(label, repr(thunk()))
    except Exception as e:
        print(label, type(e).__name__, str(e))


s = "hello world, hello"
show("startswith", lambda: s.startswith("hello"))
show("startswith tuple", lambda: s.startswith(("x", "he")))
show("startswith empty tuple", lambda: s.startswith(()))
show("startswith start", lambda: s.startswith("world", 6))
show("startswith start end", lambda: s.startswith("wor", 6, 8))
show("startswith start end exact", lambda: s.startswith("wor", 6, 9))
show("startswith neg start", lambda: s.startswith("hello", -5))
show("startswith empty", lambda: s.startswith(""))
show("startswith empty past end", lambda: s.startswith("", 100))
show("startswith empty at len", lambda: s.startswith("", len(s)))
show("''.startswith('')", lambda: "".startswith(""))
show("endswith", lambda: s.endswith("hello"))
show("endswith tuple", lambda: s.endswith(("lo", "zz")))
show("endswith end", lambda: s.endswith("world", 0, 11))
show("endswith neg end", lambda: s.endswith("o", 0, -1))
show("startswith list", lambda: s.startswith(["he"]))
show("startswith tuple with int", lambda: s.startswith(("he", 1)))
show("endswith None start", lambda: s.endswith("lo", None, None))

show("find", lambda: s.find("hello"))
show("rfind", lambda: s.rfind("hello"))
show("find missing", lambda: s.find("xyz"))
show("find start", lambda: s.find("hello", 1))
show("find start end", lambda: s.find("hello", 1, 10))
show("find neg start", lambda: s.find("l", -4))
show("find empty", lambda: s.find(""))
show("rfind empty", lambda: s.rfind(""))
show("find empty start", lambda: s.find("", 5))
show("find empty past end", lambda: s.find("", 100))
show("find empty at len", lambda: s.find("", len(s)))
show("find char", lambda: s.find(","))
show("rfind char end", lambda: s.rfind("o", 0, 8))
show("index", lambda: s.index("world"))
show("rindex", lambda: s.rindex("l"))
show("index missing", lambda: s.index("xyz"))
show("rindex missing", lambda: s.rindex("xyz"))
show("index range missing", lambda: s.index("hello", 1, 5))
show("find int", lambda: s.find(1))
show("find bytes", lambda: s.find(b"h"))
show("find non-BMP", lambda: "a😀b😀".find("b"))
show("rfind non-BMP", lambda: "a😀b😀".rfind("😀"))

show("count", lambda: s.count("hello"))
show("count char", lambda: s.count("l"))
show("count non-overlapping", lambda: "aaaa".count("aa"))
show("count overlapping text", lambda: "ababababa".count("aba"))
show("count empty", lambda: "abc".count(""))
show("count empty in empty", lambda: "".count(""))
show("count empty range", lambda: "abcdef".count("", 2, 4))
show("count start end", lambda: "aXaXaXa".count("a", 1, 5))
show("count neg", lambda: "aXaXaXa".count("a", -3))
show("count missing", lambda: "abc".count("z"))
show("count bad end", lambda: "abc".count("a", 0, 100))
show("count start > end", lambda: "abc".count("", 2, 1))

show("in", lambda: "wor" in s)
show("not in", lambda: "WOR" not in s)
show("empty in", lambda: "" in "abc")
show("empty in empty", lambda: "" in "")
show("in int", lambda: 1 in "abc")
show("in case-sensitive", lambda: "Hello" in s)
show("in non-BMP", lambda: "😀" in "a😀b")
