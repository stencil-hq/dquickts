# Miscellaneous statement syntax: semicolons, continuations, chained assignment, print options.

a = 1; b = 2; c = a + b
print("semicolons", a, b, c)
if c == 3: print("one-line if"); print("second statement in suite")

total = 1 + \
        2 + \
        3
print("backslash continuation", total)

items = [
    1,
    2,
    3,  # trailing comma and comment
]
print("implicit continuation", items)
call_result = max(
    10,
    20,
)
print("call continuation", call_result)

x = y = z = "same"
print("chained", x, y, z)
p, q = r, s = "ab"
print("chained unpack", p, q, r, s)

# expression statements are evaluated and discarded
1 + 1
"just a string"
[n for n in range(3)]
print("expression statements ok")

# print options
print("sep", 1, 2, 3, sep=", ")
print("sep empty", "a", "b", sep="")
print("end", end=" | ")
print("continues on same line")
print("no args then empty line:")
print()
print("sep None", 1, 2, sep=None)
print("end None", end=None)
print("flush", "x", flush=True)
print(*["star", "args"], sep="+")
print("multiline sep", "a", "b", sep="\n")
try:
    print("bad", sep=1)
except TypeError as e:
    print("bad sep", type(e).__name__, e)

try:
    print("bad", end=b"")
except TypeError as e:
    print("bad end", type(e).__name__, e)

# print calls str() on objects
class S:
    def __str__(self):
        return "S.__str__"

    def __repr__(self):
        return "S.__repr__"

print("str of obj", S(), [S()], (S(),), {"k": S()})

class BadStr:
    def __str__(self):
        return 5

try:
    print(BadStr())
except TypeError as e:
    print("str returns non-str", type(e).__name__, e)

# tabs vs spaces indentation errors (compile only)
for src in ["if True:\n\tx = 1\n        y = 2\n", "if True:\nx = 1\n", "  x = 1\n",
            "if True:\n    x = 1\n  y = 2\n", "x = 1\n    y = 2\n", "for i in range(3):\n"]:
    try:
        compile(src, "<i>", "exec")
        print("compiles", repr(src))
    except SyntaxError as e:
        print(type(e).__name__, repr(src), e.msg, e.lineno)

# keywords cannot be assigned
for src in ["True = 1", "None = 0", "def = 1", "class = 2", "__debug__ = 1", "f() = 1", "1 = x",
            "x + 1 = 2", "(a, b) = 1, 2", "[a, b] = 1, 2", "a.b = 1", "a[0] = 1"]:
    try:
        compile(src, "<k>", "exec")
        print("compiles", repr(src))
    except SyntaxError as e:
        print("syntax", repr(src), e.msg)

# soft keywords usable as identifiers
type = "shadowed type builtin"
print("soft keyword type", type)
del type
print("builtin type back", type(1).__name__)
_ = "underscore is a name"
print("underscore", _)

# __debug__ is True without -O
print("__debug__", __debug__)

# the builtins of the script's namespace
print("module name", __name__)

# ellipsis literal
print("ellipsis", ..., repr(...), Ellipsis is ...)

# pass in compound statements on one line
class Empty: pass
def nop(): pass
for _ in range(1): pass
while False: pass
print("one-line compound", nop(), Empty.__name__)

# long expression with many operators split over lines
value = (1
         + 2 * 3
         - 4 // 2
         ** 2)
print("multi-line expression", value)

# unicode in identifiers and strings
π = 3.14159
größe = 2
print("unicode identifiers", π, größe, π * größe)
ﬁ = "ligature normalized"
print("NFKC identifier", fi)
