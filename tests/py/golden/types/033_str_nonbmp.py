"""L0 golden: strings containing non-BMP code points behave per code point.

A VM that stores strings as UTF-16 must still report len('😀') == 1 and index,
slice, reverse, search and iterate by code point, never by surrogate half.

Usage: python3.15 -I -X utf8 tests/py/golden/types/033_str_nonbmp.py
"""


def show(label: str, thunk) -> None:
    """Print label and repr(thunk()), or the exception type and message."""
    try:
        print(label, repr(thunk()))
    except Exception as e:
        print(label, type(e).__name__, str(e))


s = "a😀b"
show("len('😀')", lambda: len("😀"))
show("len('a😀b')", lambda: len(s))
show("len('\\U0010ffff')", lambda: len("\U0010ffff"))
show("len family emoji", lambda: len("👨\u200d👩\u200d👧"))
show("len flag", lambda: len("🇫🇷"))
show("s[0]", lambda: s[0])
show("s[1]", lambda: s[1])
show("s[2]", lambda: s[2])
show("s[-1]", lambda: s[-1])
show("s[-2]", lambda: s[-2])
show("s[3]", lambda: s[3])
show("s[::-1]", lambda: s[::-1])
show("s[1:]", lambda: s[1:])
show("s[:2]", lambda: s[:2])
show("s[1:2]", lambda: s[1:2])
show("s[::2]", lambda: s[::2])
show("list(s)", lambda: list(s))
show("[ord(c) for c in s]", lambda: [hex(ord(c)) for c in s])
show("reversed", lambda: "".join(reversed("x😀y🎉z")))
show("find", lambda: "😀😀a😀".find("a"))
show("rfind", lambda: "😀😀a😀".rfind("😀"))
show("index", lambda: "a🎉b🎉c".index("c"))
show("count", lambda: "😀x😀x😀".count("😀"))
show("split", lambda: "a😀b😀c".split("😀"))
show("partition", lambda: "a😀b".partition("😀"))
show("replace", lambda: "a😀b".replace("😀", "--"))
show("upper", lambda: "a😀b".upper())
show("center", lambda: "😀".center(5, "*"))
show("ljust", lambda: "😀😀".ljust(4, "."))
show("zfill", lambda: "😀".zfill(3))
show("strip", lambda: "😀 x 😀".strip("😀 "))
show("startswith", lambda: "😀abc".startswith("😀a"))
show("endswith slice arg", lambda: "ab😀cd".endswith("😀", 0, 3))
show("in", lambda: "😀" in "a😀b")
show("half not in", lambda: "\ud83d" in "a😀b")
show("compare", lambda: "😀" > "\uffff")
show("sorted", lambda: sorted(["😀", "\uffff", "a", "\U00010000"]))
show("max", lambda: max("a😀\uffff"))
show("concat", lambda: "😀" + "😀" == "😀😀")
show("mult len", lambda: len("😀" * 10))
show("join len", lambda: len("😀".join("abc")))
show("isalpha", lambda: "😀".isalpha())
show("isidentifier math", lambda: "\U0001d465".isidentifier())
show("encode len", lambda: len(s.encode("utf-8")))
show("utf-16 len", lambda: len(s.encode("utf-16-le")))
show("chr round", lambda: chr(0x1F600) == "😀")
show("escape forms equal", lambda: "\U0001F600" == "😀" == "\N{GRINNING FACE}")
show("surrogate pair literal len", lambda: len("\ud83d\ude00"))
show("surrogate pair != emoji", lambda: "\ud83d\ude00" == "😀")
show("lone surrogate len", lambda: len("\ud800"))
show("enumerate", lambda: list(enumerate("😀🎉")))
show("slice assign via join", lambda: s[:1] + "X" + s[2:])
show("format width", lambda: format("😀", ">3"))
show("f-string width", lambda: f"[{'😀':^5}]")
show("% width", lambda: "[%3s]" % "😀")
show("dict key", lambda: {"😀": 1}["😀"])
show("set size", lambda: len(set("😀😀🎉")))
show("str.translate", lambda: "😀a".translate({0x1F600: None}))
show("iteration count", lambda: sum(1 for _ in "😀" * 7))
