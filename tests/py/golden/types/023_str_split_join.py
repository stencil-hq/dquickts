"""L0 golden: str.split, str.rsplit and str.join.

No-argument split collapses runs of (Unicode) whitespace and drops empty
strings; an explicit separator keeps empty fields.  maxsplit limits the
number of splits from the left (split) or right (rsplit).

Usage: python3.15 -I -X utf8 tests/py/golden/types/023_str_split_join.py
"""


def show(label: str, thunk) -> None:
    """Print label and repr(thunk()), or the exception type and message."""
    try:
        print(label, repr(thunk()))
    except Exception as e:
        print(label, type(e).__name__, str(e))


TEXTS = ["a b c", "  a  b  ", "", "   ", "a,b,,c,", ",a,", "a\tb\nc\r\nd\x0be\x0cf", "one",
         "a\u3000b\u00a0c\u2003d", "a\x1cb\x1dc\x1ed\x1ff", "\u200ba\u200bb"]
for t in TEXTS:
    print("split()", repr(t), t.split())
    print("rsplit()", repr(t), t.rsplit())
    print("split(',')", repr(t), t.split(","))

show("'a  b'.split(' ')", lambda: "a  b".split(" "))
show("' a b '.split(' ')", lambda: " a b ".split(" "))
show("''.split(',')", lambda: "".split(","))
show("''.split()", lambda: "".split())
show("'abc'.split('abc')", lambda: "abc".split("abc"))
show("'aXbXXc'.split('XX')", lambda: "aXbXXc".split("XX"))
show("'aaa'.split('aa')", lambda: "aaa".split("aa"))
show("'aaa'.rsplit('aa')", lambda: "aaa".rsplit("aa"))
show("'a b c d'.split(maxsplit=1)", lambda: "a b c d".split(maxsplit=1))
show("'a b c d'.rsplit(maxsplit=1)", lambda: "a b c d".rsplit(maxsplit=1))
show("'  a b c  '.split(None, 1)", lambda: "  a b c  ".split(None, 1))
show("'  a b c  '.rsplit(None, 1)", lambda: "  a b c  ".rsplit(None, 1))
show("'a,b,c,d'.split(',', 2)", lambda: "a,b,c,d".split(",", 2))
show("'a,b,c,d'.rsplit(',', 2)", lambda: "a,b,c,d".rsplit(",", 2))
show("'a,b,c'.split(',', 0)", lambda: "a,b,c".split(",", 0))
show("'a b c'.split(None, 0)", lambda: "a b c".split(None, 0))
show("'  a b c'.split(None, 0)", lambda: "  a b c".split(None, 0))
show("'a,b,c'.split(',', -1)", lambda: "a,b,c".split(",", -1))
show("'a,b'.split(sep=',', maxsplit=5)", lambda: "a,b".split(sep=",", maxsplit=5))
show("'abc'.split('')", lambda: "abc".split(""))
show("'abc'.rsplit('')", lambda: "abc".rsplit(""))
show("'abc'.split(1)", lambda: "abc".split(1))
show("'abc'.split(',', 1.5)", lambda: "abc".split(",", 1.5))
show("'a😀b😀c'.split('😀')", lambda: "a😀b😀c".split("😀"))
show("'ab'.split(b'a')", lambda: "ab".split(b"a"))

show("','.join(['a','b','c'])", lambda: ",".join(["a", "b", "c"]))
show("''.join([])", lambda: "".join([]))
show("'-'.join('abc')", lambda: "-".join("abc"))
show("'-'.join(['x'])", lambda: "-".join(["x"]))
show("', '.join(gen)", lambda: ", ".join(str(i) for i in range(5)))
show("'ab'.join(['1','2','3'])", lambda: "ab".join(["1", "2", "3"]))
show("''.join(('a', '', 'b'))", lambda: "".join(("a", "", "b")))
show("'x'.join({'k': 1})", lambda: "x".join({"k": 1, "j": 2}))
show("','.join([1, 2])", lambda: ",".join([1, 2]))
show("','.join(['a', None])", lambda: ",".join(["a", None]))
show("','.join(['a', b'b'])", lambda: ",".join(["a", b"b"]))
show("','.join(5)", lambda: ",".join(5))
show("','.join()", lambda: ",".join())
show("'😀'.join('ab')", lambda: "😀".join("ab"))
show("split/join roundtrip", lambda: ",".join("a,,b,".split(",")) == "a,,b,")
show("join many", lambda: len("".join(["ab"] * 1000)))
show("words", lambda: " ".join(reversed("the quick brown fox".split())))
show("type split result", lambda: type("a b".split()).__name__)
show("split preserves str", lambda: [type(x).__name__ for x in "a b".split()])
