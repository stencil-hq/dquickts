# String literals: quotes, raw, triple-quoted, escapes, implicit concatenation, unicode.

print("single", 'abc', "double", "abc", 'abc' == "abc")
print("quotes inside", "it's", 'say "hi"', "both ' and \"", 'esc \' quote')
print("triple", """tri "double" ple""", '''tri 'single' ple''')
tq = """line1
line2
    indented"""
print("triple multiline", repr(tq))
cont = "first \
second"
print("backslash continuation", repr(cont))

# escapes
print("simple escapes", repr("\n\t\r\\\'\"\a\b\f\v"))
print("octal", repr("\0"), repr("\101"), repr("\0101"), repr("\7"), repr("\177"))
print("hex", repr("\x41"), repr("\x7f"), repr("\xe9"), repr("\xff"))
print("unicode 16", repr("\u0041"), repr("\u00e9"), repr("\u2603"), repr("\u200b"))
print("unicode 32", repr("\U0001F600"), repr("\U00000041"), len("\U0001F600"))
print("named", repr("\N{BULLET}"), repr("\N{LATIN SMALL LETTER E WITH ACUTE}"), repr("\N{SNOWMAN}"))
print("named alias", repr("\N{NO-BREAK SPACE}"), repr("\N{EM DASH}"))
print("escapes printed", "tab[\t] bullet[\N{BULLET}] snowman[\u2603]")
print("lengths", len("\n"), len("\\n"), len("\x00"), len("\u2603"), len("é"), len("e\u0301"))
print("ord/chr", ord("\n"), ord("\N{BULLET}"), chr(8226), chr(0x1F600), ord("\0"))

# raw strings
print("raw", r"\n\t", r"C:\path\to", r"\x41", r"\N{BULLET}", len(r"\n"))
print("raw quote", r"a\"b", r'\'', R"upper R")
print("raw triple", r"""raw\ntriple""")

# implicit concatenation
s = "abc" 'def' """ghi"""
print("implicit concat", s)
print("concat with raw", "a\n" r"b\n")
multi = ("part one, "
         "part two, "
         "part three")
print("paren concat", multi)
print("concat with f", "x=" f"{1 + 1}" " done")

# unicode identifiers and text
café = "coffee"
print("unicode identifier", café)
print("non-BMP", "𝔘𝔫𝔦", len("𝔘𝔫𝔦"))
print("combining", "e\u0301", "e\u0301" == "é")
print("case", "ß".upper(), "İ".lower(), len("İ".lower()), "ǅ".swapcase())

# string prefix forms
print("u prefix", u"unicode", U"upper", type(u"x").__name__)
print("rb/br bytes", rb"\n", Br"\x", bR"a")

# invalid escapes / syntax errors (compiled, not executed directly)
for src in [r'"\x4"', r'"\u12"', r'"\U1234"', r'"\N{NOT A REAL NAME}"', r'"\N"', '"unterminated',
            '"""unterminated', "'a' 'b", r'b"\u0041"', 'b"é"', r'"\777"', r'"\8"']:
    try:
        code = compile(src, "<lit>", "eval")
        print("compiles", src, repr(eval(code)))
    except SyntaxError as e:
        print("syntax", src, e.msg)

# str() and repr() of strings with special content
for v in ["", " ", "\x00", "\x7f", "\x80", "\xa0", "\u2028", "\ud800", "\U0010ffff", "\x1b[0m"]:
    print("repr", repr(v), ascii(v), len(v))

# string multiplication and concatenation
print("mul", "ab" * 3, "x" * 0, "x" * -1, 3 * "-", repr("" * 100))
print("concat", "a" + "b" + "", "a" + str(1))
try:
    "a" + 1
except TypeError as e:
    print("str + int", type(e).__name__, e)

try:
    "a" * 1.5
except TypeError as e:
    print("str * float", type(e).__name__, e)

# docstrings are just string literals
def documented():
    """Doc line.

    More text."""

print("docstring", repr(documented.__doc__))
