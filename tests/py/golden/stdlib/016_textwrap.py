"""L0 golden: textwrap dedent/indent/wrap/fill/shorten."""
import textwrap

para = (
    "The quick brown fox jumps over the lazy dog. Pack my box with five dozen "
    "liquor jugs. How vexingly quick daft zebras jump!"
)

print("wrap 30", textwrap.wrap(para, 30))
print("wrap 10", textwrap.wrap("supercalifragilistic word", 10))
print("wrap no break long", textwrap.wrap("supercalifragilistic word", 10, break_long_words=False))
print("wrap hyphen", textwrap.wrap("well-known state-of-the-art", 12))
print("wrap no hyphen break", textwrap.wrap("well-known state-of-the-art", 12, break_on_hyphens=False))
print("wrap empty", textwrap.wrap(""), textwrap.wrap("   "))
print("wrap whitespace", textwrap.wrap("a\tb\nc   d", 80))
print("wrap tabsize", textwrap.wrap("a\tb", 80, expand_tabs=True, tabsize=4, replace_whitespace=False))
print("wrap max_lines", textwrap.wrap(para, 25, max_lines=2))
print("wrap max_lines placeholder", textwrap.wrap(para, 25, max_lines=2, placeholder=" [more]"))
print("wrap drop ws off", textwrap.wrap("  lead  and  trail  ", 8, drop_whitespace=False))
print("fix_sentence", textwrap.wrap("End. Next one.", 80, fix_sentence_endings=True))
try:
    textwrap.wrap("x", 0)
except ValueError as e:
    print("width 0", type(e).__name__, e)

print("fill 40:")
print(textwrap.fill(para, 40))
print("fill indents:")
print(textwrap.fill(para, 36, initial_indent="* ", subsequent_indent="  "))

w = textwrap.TextWrapper(width=20, initial_indent="> ", subsequent_indent="> ")
print("TextWrapper:")
print(w.fill("one two three four five six seven eight nine ten"))
print("TextWrapper wrap", w.wrap("short"))

code = """
    def f(x):
        if x:
            return 1
        return 0
"""
print("dedent:")
print(textwrap.dedent(code), end="")
print("dedent mixed", repr(textwrap.dedent("  a\n    b\n  c")))
print("dedent blank lines", repr(textwrap.dedent("    a\n\n    b\n")))
print("dedent ws-only line", repr(textwrap.dedent("  a\n      \n  b")))
print("dedent tabs vs spaces", repr(textwrap.dedent("\ta\n  b")))
print("dedent none", repr(textwrap.dedent("a\n  b")))

print("indent", repr(textwrap.indent("a\nb\n\nc\n", "  ")))
print("indent pred", repr(textwrap.indent("a\n\nb", "# ", lambda line: True)))
print("indent pred odd", repr(textwrap.indent("x\ny\nz\n", "-", lambda line: line.startswith("y"))))
print("indent crlf", repr(textwrap.indent("a\r\nb\r\n", "> ")))

print("shorten", repr(textwrap.shorten(para, 30)))
print("shorten fits", repr(textwrap.shorten("Hello  world!", 20)))
print("shorten placeholder", repr(textwrap.shorten(para, 25, placeholder="...")))
print("shorten collapse", repr(textwrap.shorten("  many    spaces   here ", 100)))
try:
    textwrap.shorten("hello world", 3)
except ValueError as e:
    print("shorten too small", type(e).__name__, e)
print("shorten word only", repr(textwrap.shorten("Supercalifragilistic", 10)))

for width in (5, 15, 25):
    lines = textwrap.wrap("aa bb cc dd ee ff gg hh ii", width)
    print("widths", width, lines, max(len(x) for x in lines))
