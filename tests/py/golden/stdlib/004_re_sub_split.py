"""L0 golden: re.sub/subn/split and replacement templates."""
import re

s = "John Smith, Jane Doe, Bob Lee"

print("sub plain", re.sub(r"\s+", " ", "a   b \t\n c"))
print("sub backref", re.sub(r"(\w+) (\w+)", r"\2 \1", s))
print("sub named", re.sub(r"(?P<first>\w+) (?P<last>\w+)", r"\g<last>, \g<first>", s))
print("sub g0", re.sub(r"\d+", r"<\g<0>>", "a1b22c333"))
print("sub g num", re.sub(r"(a)", r"\g<1>0", "aa"))
print("sub escape", re.sub(r"x", r"\\n", "axb"), re.sub(r"x", "\n", "axb") == "a\nb")
print("sub tab template", repr(re.sub(r"x", r"\t", "x")))
print("sub count", re.sub(r"o", "0", "foo boo zoo", count=2))
print("sub empty matches", re.sub(r"x*", "-", "abc"))
print("sub empty after match", re.sub(r"a*", "-", "baac"))
print("sub no match", re.sub(r"q", "Q", "abc"))
print("sub unmatched group", repr(re.sub(r"(a)|(b)", r"[\1\2]", "ab")))


def shout(m):
    return m.group().upper() + "!"


print("sub func", re.sub(r"\b\w{3}\b", shout, "the cat sat on a mat"))
print("sub lambda", re.sub(r"\d+", lambda m: str(int(m.group()) * 2), "1 + 20 = 21"))
print("sub func groups", re.sub(r"(\w)(\w*)", lambda m: m[1].upper() + m[2], "hello big world"))

print("subn", re.subn(r"a", "A", "banana"))
print("subn count", re.subn(r"a", "A", "banana", count=1))
print("subn none", re.subn(r"z", "Z", "banana"))

p = re.compile(r"(\d+)")
print("pattern sub", p.sub(r"#\1", "x1y22"))
print("pattern subn", p.subn("N", "1 2 3", count=2))

# bad templates
for tmpl in [r"\3", r"\g<nope>", r"\g<1", "\\"]:
    try:
        re.sub(r"(a)(b)", tmpl, "ab")
    except (re.error, IndexError) as e:
        print("bad template", repr(tmpl), type(e).__name__, e)

# split
print("split ws", re.split(r"\s+", "a b  c\td"))
print("split none", re.split(r"\s+", "abc"))
print("split capture", re.split(r"(,)", "a,b,c"))
print("split 2 captures", re.split(r"(-)|(\+)", "1-2+3"))
print("split maxsplit", re.split(r",", "a,b,c,d", maxsplit=2))
print("split maxsplit capture", re.split(r"(\d)", "a1b2c3d", maxsplit=1))
print("split leading", re.split(r",", ",a,,b,"))
print("split empty pattern", re.split(r"", "abc"))
print("split zero width", re.split(r"(?=[A-Z])", "HelloBigWorld"))
print("split flags", re.split(r"x", "aXbxc", flags=re.I))
print("pattern split", re.compile(r"[;,]\s*").split("a; b,c ,d"))
print("str split vs re split", "a  b".split(" "), re.split(" ", "a  b"))

# escape
print("escape", re.escape("a.b*c?(d)[e]{f}|g^h$i\\j+k"))
print("escape safe", re.escape("abc_123 -/"))
print("escape unicode", re.escape("ü€"))
pattern = re.escape("1+1=2?")
print("escape use", bool(re.fullmatch(pattern, "1+1=2?")))
print("escaped sub", re.sub(re.escape("$x"), "VAL", "cost: $x, $xy"))
