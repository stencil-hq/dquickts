"""L0 golden: re flags, inline flags, groups, lookaround, Unicode classes, errors."""
import re

# flags
print("I", re.findall(r"abc", "ABC abc AbC", re.I))
print("M", re.findall(r"^\w+$", "one\ntwo\nthree", re.M))
print("no M", re.findall(r"^\w+$", "one\ntwo"))
print("S", re.search(r"a.b", "a\nb", re.S), re.search(r"a.b", "a\nb"))
print("combined", re.findall(r"^x.y$", "X\nY\nx-y", re.I | re.M | re.S))
print("flag values", int(re.I), int(re.M), int(re.S), int(re.X), int(re.A))
print("flags repr", repr(re.I | re.M))
print("pattern flags", re.compile("a", re.I).flags & re.I == re.I)

verbose = re.compile(
    r"""
    (?P<year>\d{4})   # year
    -                 # separator
    (?P<month>\d{2})  # month
    (?:-(?P<day>\d{2}))?  # optional day
    \ \#tag           # escaped space and hash
    """,
    re.X,
)
print("verbose", verbose.match("2024-05-17 #tag").groupdict())
print("verbose no day", verbose.match("2024-05 #tag").groupdict())
print("verbose fail", verbose.match("2024-05-17#tag"))

# inline flags
print("inline i", re.findall(r"(?i)hello", "Hello HELLO hello"))
print("scoped i", re.findall(r"(?i:h)ello", "Hello HELLO hello"))
print("scoped off", re.findall(r"(?i)a(?-i:b)", "ab AB aB Ab"))
print("inline s", re.search(r"(?s)a.c", "a\nc").group() == "a\nc")
print("inline m", re.findall(r"(?m)^\d", "1\n2\n3"))
print("inline x", re.match(r"(?x) a b c  # comment", "abc"))
try:
    re.compile(r"a(?i)b")
except re.error as e:
    print("global flag not at start", type(e).__name__, e)

# named groups and backrefs
print("named backref", re.search(r"(?P<w>\w+) (?P=w)", "say hello hello there").group())
print("numbered backref", re.findall(r"(\w)\1", "aabbcd"))
print("non-capturing", re.match(r"(?:ab)+(c)", "ababc").groups())
print("nested groups", re.match(r"((a)(b(c)))", "abc").groups())
print("alternation group", re.match(r"(cat|dog)s?", "dogs").group(1))
print("conditional", [bool(re.fullmatch(r"(<)?\w+(?(1)>)", t)) for t in ["<a>", "a", "<a", "a>"]])

# lookaround
print("lookahead", re.findall(r"\w+(?=,)", "a, bb, ccc"))
print("neg lookahead", re.findall(r"\b\d+\b(?!%)", "10% 20 30%"))
print("lookbehind", re.findall(r"(?<=\$)\d+", "$10 20 $30"))
print("neg lookbehind", re.findall(r"(?<!-)\b\d+", "-1 2 -3 4"))
print("password", [bool(re.match(r"(?=.*\d)(?=.*[A-Z]).{6,}", p)) for p in ["abcdef", "Abcde1", "A1"]])
try:
    re.compile(r"(?<=a+)b")
except re.error as e:
    print("variable lookbehind", type(e).__name__, e)

# Unicode-aware classes
u = "x٣5 café_ß Ω\u00a0\u2003z"
print("digits unicode", re.findall(r"\d", u))
print("digits ascii", re.findall(r"\d", u, re.ASCII))
print("words unicode", re.findall(r"\w+", u))
print("words ascii", re.findall(r"\w+", u, re.A))
print("spaces unicode", [hex(ord(c)) for c in re.findall(r"\s", u)])
print("spaces ascii", [hex(ord(c)) for c in re.findall(r"\s", u, re.A)])
print("ignorecase unicode", re.findall(r"ß|σ", "SS Σ σ ς", re.I))
print("ignorecase k", re.findall(r"k", "K k \u212a", re.I), re.findall(r"k", "K k \u212a", re.I | re.A))
print("inline a", re.findall(r"(?a)\w+", "café"))
print("bytes pattern", re.findall(rb"\w+", b"caf\xc3\xa9 ok"))
try:
    re.search(r"a", b"a")
except TypeError as e:
    print("str/bytes mix", type(e).__name__, e)
try:
    re.compile(rb"\w", re.U)
except ValueError as e:
    print("bytes unicode flag", type(e).__name__, e)

# errors
for bad in ["(", ")", "[a-", "a**", "*a", r"(?P<1>x)", r"(?P<n>a)(?P<n>b)", r"\p", "a{3,1}", r"(?P=missing)", "[z-a]", r"\1"]:
    try:
        re.compile(bad)
        print("compiled", repr(bad))
    except re.error as e:
        print("error", repr(bad), "->", e.msg, "| pos", e.pos)
print("error is", re.error.__name__, re.PatternError is re.error)
