"""L0 golden: str.replace and the strip family.

replace substitutes all occurrences unless count is given; an empty `old`
inserts between every character.  strip without arguments removes Unicode
whitespace; with an argument removes any of the given characters.

Usage: python3.15 -I -X utf8 tests/py/golden/types/025_str_replace_strip.py
"""


def show(label: str, thunk) -> None:
    """Print label and repr(thunk()), or the exception type and message."""
    try:
        print(label, repr(thunk()))
    except Exception as e:
        print(label, type(e).__name__, str(e))


show("replace all", lambda: "a-b-c-d".replace("-", "+"))
show("replace count 2", lambda: "a-b-c-d".replace("-", "+", 2))
show("replace count 0", lambda: "a-b-c-d".replace("-", "+", 0))
show("replace count -1", lambda: "a-b-c-d".replace("-", "+", -1))
show("replace count kw", lambda: "aaaa".replace("a", "b", count=3))
show("replace missing", lambda: "abc".replace("x", "y"))
show("replace to empty", lambda: "banana".replace("a", ""))
show("replace grow", lambda: "abc".replace("b", "BBBB"))
show("replace overlap", lambda: "aaaa".replace("aa", "b"))
show("replace overlap 2", lambda: "aaa".replace("aa", "X"))
show("replace empty old", lambda: "abc".replace("", "-"))
show("replace empty old count", lambda: "abc".replace("", "-", 2))
show("replace empty in empty", lambda: "".replace("", "x"))
show("replace in empty", lambda: "".replace("a", "x"))
show("replace whole", lambda: "abc".replace("abc", ""))
show("replace non-BMP", lambda: "a😀b😀".replace("😀", "!"))
show("replace into non-BMP", lambda: "a.b".replace(".", "\U0001f600"))
show("replace ß", lambda: "straße".replace("ß", "ss"))
show("replace bad type", lambda: "abc".replace(1, "x"))
show("replace bad count", lambda: "abc".replace("a", "x", "1"))
show("replace is new obj eq", lambda: "abc".replace("z", "y") == "abc")

show("strip", lambda: "  hi  ".strip())
show("lstrip", lambda: "  hi  ".lstrip())
show("rstrip", lambda: "  hi  ".rstrip())
show("strip mixed ws", lambda: "\t\n\r\x0b\x0c hi \x1c\x1d\x1e\x1f\x85".strip())
show("strip unicode spaces", lambda: "\u00a0\u2000\u3000hi\u2028\u2029\u202f".strip())
show("strip zero width (not ws)", lambda: "\u200bhi\u200b".strip())
show("strip chars", lambda: "xxhixyx".strip("xy"))
show("lstrip chars", lambda: "xxhixyx".lstrip("xy"))
show("rstrip chars", lambda: "xxhixyx".rstrip("xy"))
show("strip chars order", lambda: "abcHELLOcba".strip("cab"))
show("strip None", lambda: "  x ".strip(None))
show("strip empty chars", lambda: "  x ".strip(""))
show("strip all", lambda: "aaaa".strip("a"))
show("strip empty str", lambda: "".strip())
show("strip only ws", lambda: " \t\n ".strip())
show("strip non-BMP chars", lambda: "😀x😀".strip("😀"))
show("rstrip newline", lambda: "line\r\n".rstrip("\r\n"))
show("strip not substring", lambda: "www.example.com".strip("w.moc"))
show("strip bad type", lambda: "x".strip(1))
show("strip too many", lambda: "x".strip("a", "b"))
show("lstrip kw", lambda: "x".lstrip(chars="x"))
show("removeprefix vs lstrip", lambda: ("abcabcX".lstrip("abc"), "abcabcX".removeprefix("abc")))
show("chained", lambda: "  A-B  ".strip().replace("-", " ").lower())
