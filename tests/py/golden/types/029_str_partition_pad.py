"""L0 golden: partition/rpartition, padding, expandtabs and prefix/suffix removal.

Usage: python3.15 -I -X utf8 tests/py/golden/types/029_str_partition_pad.py
"""


def show(label: str, thunk) -> None:
    """Print label and repr(thunk()), or the exception type and message."""
    try:
        print(label, repr(thunk()))
    except Exception as e:
        print(label, type(e).__name__, str(e))


show("partition", lambda: "key=value=x".partition("="))
show("rpartition", lambda: "key=value=x".rpartition("="))
show("partition missing", lambda: "abc".partition("="))
show("rpartition missing", lambda: "abc".rpartition("="))
show("partition at start", lambda: "=abc".partition("="))
show("partition at end", lambda: "abc=".partition("="))
show("partition multi-char", lambda: "a::b::c".partition("::"))
show("rpartition multi-char", lambda: "a::b::c".rpartition("::"))
show("partition empty str", lambda: "".partition("x"))
show("partition empty sep", lambda: "abc".partition(""))
show("rpartition empty sep", lambda: "abc".rpartition(""))
show("partition None", lambda: "abc".partition(None))
show("partition type", lambda: type("a=b".partition("=")).__name__)

show("center", lambda: "abc".center(9))
show("center even pad", lambda: "abc".center(6))
show("center even pad 2", lambda: "ab".center(5))
show("center fill", lambda: "abc".center(9, "*"))
show("center small", lambda: "abc".center(2))
show("center negative", lambda: "abc".center(-5))
show("ljust", lambda: "abc".ljust(6) + "|")
show("ljust fill", lambda: "abc".ljust(6, "."))
show("rjust", lambda: "abc".rjust(6))
show("rjust fill", lambda: "abc".rjust(6, "0"))
show("rjust non-BMP fill", lambda: "x".rjust(3, "😀"))
show("center fill too long", lambda: "abc".center(9, "ab"))
show("center fill empty", lambda: "abc".center(9, ""))
show("ljust bad width", lambda: "abc".ljust("5"))
show("ljust float width", lambda: "abc".ljust(5.0))
show("center width only odd", lambda: "a".center(4, "-"))
show("center width only odd 2", lambda: "aa".center(5, "-"))

show("zfill", lambda: "42".zfill(5))
show("zfill negative", lambda: "-42".zfill(5))
show("zfill plus", lambda: "+42".zfill(5))
show("zfill short", lambda: "12345".zfill(3))
show("zfill empty", lambda: "".zfill(3))
show("zfill only sign", lambda: "-".zfill(3))
show("zfill text", lambda: "abc".zfill(6))
show("zfill float", lambda: "-3.5".zfill(8))
show("zfill double sign", lambda: "--1".zfill(5))

show("expandtabs default", lambda: "a\tbc\tdef\tg".expandtabs())
show("expandtabs 4", lambda: "a\tbc\tdef\tg".expandtabs(4))
show("expandtabs 0", lambda: "a\tb".expandtabs(0))
show("expandtabs 1", lambda: "a\tb".expandtabs(1))
show("expandtabs neg", lambda: "a\tb".expandtabs(-1))
show("expandtabs newline resets", lambda: "ab\tc\nd\te".expandtabs(4))
show("expandtabs cr resets", lambda: "ab\tc\rd\te".expandtabs(4))
show("expandtabs leading", lambda: "\t\tx".expandtabs(3))
show("expandtabs kw", lambda: "a\tb".expandtabs(tabsize=2))

show("removeprefix", lambda: "TestCase".removeprefix("Test"))
show("removeprefix missing", lambda: "TestCase".removeprefix("Case"))
show("removeprefix once", lambda: "aaab".removeprefix("a"))
show("removeprefix empty", lambda: "abc".removeprefix(""))
show("removeprefix whole", lambda: "abc".removeprefix("abc"))
show("removesuffix", lambda: "file.tar.gz".removesuffix(".gz"))
show("removesuffix missing", lambda: "file.tar.gz".removesuffix(".zip"))
show("removesuffix empty", lambda: "abc".removesuffix(""))
show("removesuffix longer", lambda: "c".removesuffix("abc"))
show("removeprefix tuple", lambda: "abc".removeprefix(("a",)))
show("removesuffix None", lambda: "abc".removesuffix(None))
