"""L0 golden: re search/match/fullmatch/findall/finditer and the Match API."""
import re

text = "Order 66: 12 apples, 7 pears, 300 plums."

print("search", re.search(r"\d+", text))
print("search none", re.search(r"xyz", text))
print("match none", re.match(r"\d+", text))
print("match", re.match(r"Order", text))
print("fullmatch", re.fullmatch(r"\w+", "abc123"))
print("fullmatch none", re.fullmatch(r"\w+", "abc 123"))
print("match prefix only", re.match(r"ab", "abc").group())

# findall with 0/1/many groups
print("findall 0 groups", re.findall(r"\d+", text))
print("findall 1 group", re.findall(r"(\d+) \w+", text))
print("findall 2 groups", re.findall(r"(\d+) (\w+)", text))
print("findall opt group", re.findall(r"(a)?(b)", "ab b"))
print("findall empty", re.findall(r"x*", "axb"))
print("findall none", re.findall(r"z", "abc"))

# finditer spans
for m in re.finditer(r"[a-z]+s\b", text):
    print("finditer", repr(m.group()), m.span(), m.start(), m.end())
print("finditer empty", [m.span() for m in re.finditer(r"", "ab")])

# Match API
m = re.search(r"(?P<qty>\d+) (?P<fruit>\w+)(?P<suffix>z)?", text[10:])
print("group0", repr(m.group(0)), repr(m[0]))
print("group1", m.group(1), m["fruit"])
print("group multi", m.group(1, 2))
print("groups", m.groups())
print("groups default", m.groups("-"))
print("groupdict", m.groupdict())
print("groupdict default", m.groupdict(default="?"))
print("span", m.span(), m.span(1), m.span("fruit"), m.span(3))
print("start/end", m.start(2), m.end(2), m.start("suffix"))
print("lastindex", m.lastindex, "lastgroup", m.lastgroup)
print("string/pos", repr(m.string[:5]), m.pos, m.endpos)
print("re attr", m.re.pattern)
print("expand", m.expand(r"\2=\1 [\g<qty>]"))
try:
    m.group(9)
except IndexError as e:
    print("bad group", type(e).__name__, e)
try:
    m.group("nope")
except IndexError as e:
    print("bad name", type(e).__name__, e)

m2 = re.match(r"(a)(b)?", "ac")
print("unmatched group", m2.groups(), m2.span(2), m2.lastindex)
print("bool", bool(re.match("a", "a")), bool(re.match("b", "a")))

# compiled patterns and pos/endpos
p = re.compile(r"\d")
print("pattern repr", repr(p))
print("pattern attrs", p.pattern, p.groups, p.groupindex)
print("pos endpos", p.search("a1b2c3", 2), p.findall("a1b2c3", 2, 5))
print("match at pos", p.match("ab3", 2))
q = re.compile(r"(?P<a>x)(?P<b>y)")
print("groupindex", dict(q.groupindex), q.groups)

# greedy vs lazy
html = "<b>bold</b><i>it</i>"
print("greedy", re.findall(r"<.*>", html))
print("lazy", re.findall(r"<.*?>", html))
print("lazy plus", re.search(r"a+?", "aaa").group())
print("lazy brace", re.search(r"a{2,4}?", "aaaaa").group(), re.search(r"a{2,4}", "aaaaa").group())
print("possessive", re.search(r"a++b", "aaab").group(), re.search(r"a*+a", "aaa"))
print("atomic", re.search(r"(?>a+)b", "aab"), re.search(r"(?>a+)a", "aaa"))

# anchors
print("dollar before nl", re.search(r"end$", "the end\n"))
print("Z before nl", re.search(r"end\Z", "the end\n"))
print("Z", re.search(r"end\Z", "the end"))
print("A", re.findall(r"\A\w", "ab\ncd", re.M), re.findall(r"^\w", "ab\ncd", re.M))
print("word boundary", re.findall(r"\bcat\b", "cat concat cat. bobcat"))
print("non boundary", re.findall(r"\Bcat", "cat concat bobcat"))
