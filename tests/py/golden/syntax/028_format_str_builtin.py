# Format specs on strings and the format() builtin with various objects.

words = ["", "a", "abc", "hello world", "é✓", "tab\tx"]
specs = ["", "s", ">8", "<5", "^9", "*^9", "-<6", ".2", ">8.2", "^7.1", "=>4", "05", "<0"]
for w in words:
    print("str", repr(w), " ".join("[" + format(w, s) + "]" for s in specs))

# alignment helpers vs format
s = "mid"
print("ljust/rjust/center", repr(s.ljust(7, ".")), repr(s.rjust(7, ".")), repr(s.center(8, ".")))
print("format eq", format(s, ".<7") == s.ljust(7, "."), format(s, ".>7") == s.rjust(7, "."))
print("center odd", repr(format("ab", "^5")), repr("ab".center(5)), repr(format("abc", "^6")), repr("abc".center(6)))

# string format errors
def fmt(v, spec):
    try:
        print("fmt", repr(v), repr(spec), repr(format(v, spec)))
    except (ValueError, TypeError) as e:
        print("fmt", repr(v), repr(spec), type(e).__name__, e)

fmt("x", "d")
fmt("x", "+")
fmt("x", ",")
fmt("x", "#")
fmt("x", "=5")
fmt("x", "z")
fmt("x", "10.3s")
fmt("x", "\u2603^5")

# format() builtin with other types
fmt(None, "")
fmt(None, ">6")
fmt([1, 2], "")
fmt([1, 2], ">10")
fmt((1,), "")
fmt({"a": 1}, "")
fmt(True, "")
fmt(True, ">6")
fmt(b"ab", "")
fmt(range(3), "")
fmt(3 + 4j, ">12")
fmt(object, "")

# format() with no spec calls __format__('') which defaults to str()
class Thing:
    def __str__(self):
        return "Thing!"

    def __repr__(self):
        return "Thing()"

print("default __format__", format(Thing()), f"{Thing()}", "{}".format(Thing()))
fmt(Thing(), "")
fmt(Thing(), ">10")

class Custom:
    def __format__(self, spec):
        return "custom[" + spec + "]"

print("custom", format(Custom(), "anything"), f"{Custom():xy}", "{:z}".format(Custom()))

class BadFormat:
    def __repr__(self):
        return "BadFormat()"

    def __format__(self, spec):
        return 42

fmt(BadFormat(), "")

class Raises:
    def __format__(self, spec):
        raise KeyError(spec)

try:
    format(Raises(), "k")
except KeyError as e:
    print("format raises", type(e).__name__, e)

# format() argument errors
try:
    format()
except TypeError as e:
    print("format no args", type(e).__name__, e)

try:
    format(1, 2)
except TypeError as e:
    print("format non-str spec", type(e).__name__, e)

# str subclasses
class MyStr(str):
    pass

print("subclass", repr(format(MyStr("q"), ">3")), type(format(MyStr("q"), "")).__name__)

class LoudStr(str):
    def __format__(self, spec):
        return self.upper() + "!" + spec

print("subclass override", format(LoudStr("hey"), "5"), f"{LoudStr('x'):>3}")

# wide fills and unicode
print("unicode fill", f"[{'x':→^7}]", f"[{'日本':<6}]|", len(f"{'日本':<6}"))
print("truncate", f"[{'abcdef':.3}]", f"[{'abcdef':>8.3}]", f"[{'ab':.10}]")
print("format type", type(format(5, "")).__name__, type(format("s", "")).__name__)
