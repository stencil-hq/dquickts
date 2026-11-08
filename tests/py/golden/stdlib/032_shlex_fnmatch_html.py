"""L0 golden: shlex, fnmatch and html escaping."""
import fnmatch
import html
import html.entities
import shlex

# shlex
for cmd in [
    "ls -la /tmp",
    'echo "hello world" \'single q\' plain',
    r"a\ b c\"d",
    'mix"ed quo"tes',
    "  leading   spaces  ",
    "x # comment here",
    "",
    "'it'\"'\"'s'",
    r'"esc \" inside" "\$HOME"',
]:
    print("split", repr(cmd), shlex.split(cmd))
print("split comments", shlex.split("x # comment", comments=True))
print("split non-posix", shlex.split('"a b" c', posix=False))
for bad in ['"unterminated', "'also", "trailing\\"]:
    try:
        shlex.split(bad)
    except ValueError as e:
        print("split error", repr(bad), type(e).__name__, e)

for s in ["simple", "has space", "", "it's", "$var", "a;b", "safe-chars_./@%+=:,", "ü"]:
    print("quote", repr(s), shlex.quote(s))
args = ["cp", "my file.txt", "it's here", "-v"]
joined = shlex.join(args)
print("join", joined)
print("join roundtrip", shlex.split(joined) == args)

lex = shlex.shlex("a = b + 'c d';", posix=True)
lex.whitespace_split = False
print("lexer tokens", list(lex))
lex2 = shlex.shlex("key=value other=x", posix=True)
lex2.whitespace_split = True
print("lexer ws split", list(lex2))
lex3 = shlex.shlex("a&&b||c;d", punctuation_chars=True)
print("punctuation_chars", list(lex3))

# fnmatch (case-sensitive on POSIX via fnmatchcase)
names = ["main.py", "test_main.py", "README.md", "setup.cfg", "data1.csv", "data22.csv", ".hidden", "Main.PY"]
for pat in ["*.py", "test_*", "data?.csv", "data[0-9]*.csv", "[!.]*", "*.[ch]fg", "*", "[A-Z]*", "*.PY"]:
    print("filter", pat, fnmatch.filter(names, pat))
print("fnmatchcase", fnmatch.fnmatchcase("Main.PY", "*.py"), fnmatch.fnmatchcase("a.py", "*.py"))
print("fnmatch posix", fnmatch.fnmatch("FOO.txt", "foo.*"), fnmatch.fnmatch("foo.txt", "foo.*"))
print("special chars", fnmatch.fnmatchcase("a[b]", "a[[]b]"), fnmatch.fnmatchcase("a*", "a[*]"), fnmatch.fnmatchcase("a?", "a[?]"))
print("star matches slash", fnmatch.fnmatchcase("dir/file.py", "*.py"))
print("empty", fnmatch.fnmatchcase("", "*"), fnmatch.fnmatchcase("", "?"))
print("bad range", fnmatch.fnmatchcase("b", "[z-a]"), fnmatch.fnmatchcase("[", "["))
print("translate", fnmatch.translate("*.py"), fnmatch.translate("a?[!0-9]"))
print("filterfalse", fnmatch.filterfalse(names, "*.py"))

# html
s = """<a href="x?a=1&b=2">Tom's "quote"</a>"""
print("escape", html.escape(s))
print("escape no quote", html.escape(s, quote=False))
print("escape empty", repr(html.escape("")))
for t in ["&lt;p&gt;", "&amp;amp;", "&#65;&#x42;&#x43", "&copy; &eacute; &euro;", "&nosuch;", "&amp", "&#0;", "&#x110000;",
          "&lt", "AT&T", "&notit;", "&#128;"]:
    print("unescape", repr(t), repr(html.unescape(t)))
print("roundtrip", html.unescape(html.escape(s)) == s)
print("entities", html.entities.name2codepoint["eacute"], html.entities.codepoint2name[0x3B1], html.entities.html5["gt;"])
