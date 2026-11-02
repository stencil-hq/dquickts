# str.format: auto/manual numbering, keywords, indexing, attributes, conversions, errors.

print("auto", "{} + {} = {}".format(1, 2, 3))
print("manual", "{1} {0} {1}".format("a", "b"))
print("keyword", "{name} is {age}".format(name="Bo", age=7))
print("mixed", "{0} {x} {1}".format("p", "q", x="kw"))
print("repeat", "{0}{0}{0}".format("ab"))
print("unused args ok", "{0}".format(1, 2, 3), "{a}".format(a=1, b=2))
print("escaped braces", "{{}} {{{0}}} }}{{".format("v"))
print("empty", "".format(1), "no fields".format())

# indexing and attribute access
data = {"key": "value", "1": "str-one", 1: "int-one"}
lst = ["zero", "one", "two"]
print("index list", "{0[1]} {0[2]}".format(lst))
print("index dict str key", "{0[key]}".format(data))
print("index digit key is int", "{0[1]}".format(data))
print("keyword index", "{d[key]} {l[0]}".format(d=data, l=lst))
print("nested index", "{0[0][1]}".format([["a", "b"]]))

class Point:
    def __init__(self, x, y):
        self.x = x
        self.y = y

p = Point(3, -4)
print("attribute", "({0.x}, {0.y})".format(p))
print("keyword attribute", "{pt.x}|{pt.y}".format(pt=p))
print("attribute then index", "{0.items[1]}".format(type("Obj", (), {"items": [10, 20]})))
print("method attr str", "{0.real} {0.imag}".format(3 + 4j))

# conversions and specs
s = "café"
print("conversions", "{0!s} {0!r} {0!a}".format(s))
print("conversion with spec", "[{0!r:>10}]".format("x"))
print("spec", "[{:>6}] [{:<6}] [{:^6}] [{:06.2f}] [{:,}] [{:#x}]".format("r", "l", "c", 3.14159, 10**6, 255))
print("nested spec", "[{:{}.{}f}]".format(2.71828, 10, 3))
print("nested spec named", "[{v:{w}}]".format(v="x", w=5))
print("nested spec numbered", "[{0:{1}{2}}]".format("y", "*^", 7))

# format_map
class Default(dict):
    def __missing__(self, key):
        return "<" + key + ">"

print("format_map", "{a} {b}".format_map(Default(a="A")))
print("format_map dict", "{x}".format_map({"x": 1}))

# errors
def attempt(label, fmt, *args, **kwargs):
    try:
        print(label, repr(fmt.format(*args, **kwargs)))
    except Exception as e:
        print(label, type(e).__name__, e)

attempt("index out of range", "{2}", "a", "b")
attempt("auto too few", "{} {}", "a")
attempt("missing keyword", "{name}", nm=1)
attempt("mix auto manual", "{} {0}", "a")
attempt("mix manual auto", "{0} {}", "a")
attempt("single open brace", "{", 1)
attempt("single close brace", "}", 1)
attempt("unclosed field", "{0", 1)
attempt("bad conversion", "{0!x}", 1)
attempt("bad spec", "{0:q}", 1)
attempt("missing attr", "{0.nope}", p)
attempt("missing key", "{0[nokey]}", {})
attempt("list str index", "{0[a]}", [1])
attempt("index out of list", "{0[5]}", [1])
attempt("empty attribute", "{0.}", p)
attempt("too deep nesting", "{:{:{}}}", 1, 2, 3)
attempt("format int with s", "{:s}", 5)
attempt("keyword not identifier ok", "{a-b}", **{"a-b": "dash"})
attempt("space in name", "{ x }", **{" x ": "spaced"})
attempt("negative index is a key", "{0[-1]}", {"-1": "minus"})

# format on non-str objects with __format__
class Money:
    def __init__(self, cents):
        self.cents = cents

    def __format__(self, spec):
        value = "%d.%02d" % divmod(self.cents, 100)
        return format("$" + value, spec)

print("custom __format__", "{:>10}|{}".format(Money(12345), Money(5)))

# str.format on bytes does not exist
try:
    b"{}".format(1)
except AttributeError as e:
    print("bytes format", type(e).__name__, e)

# format returns new str; original unchanged
tmpl = "{}-{}"
out = tmpl.format(1, 2)
print("template intact", tmpl, out)
