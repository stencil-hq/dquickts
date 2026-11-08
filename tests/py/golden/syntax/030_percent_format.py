# printf-style %-formatting on str (and bytes): conversions, flags, tuple/dict operands, errors.

print("%s", "%s" % "text", "%s" % 42, "%s" % None, "%s" % [1, "a"])
print("%r", "%r" % "text", "%r" % 42, "%r" % "it's")
print("%a", "%a" % "café")
print("%d", "%d" % 42, "%d" % -7, "%d" % 3.99, "%d" % -3.99, "%d" % True, "%i" % 5)
print("%5d", "[%5d]" % 42, "[%-5d]" % 42, "[%05d]" % 42, "[%+d]" % 42, "[% d]" % 42, "[%+05d]" % -42)
print("%x", "%x" % 255, "%X" % 255, "%#x" % 255, "%#X" % 255, "%x" % -255, "%08x" % 3054)
print("%o", "%o" % 8, "%#o" % 8, "%#o" % 0)
print("%f", "%f" % 3.14159, "%.2f" % 3.14159, "%5.2f" % 3.14159, "%-8.3f|" % 2.5, "%08.3f" % -2.5)
print("%.0f", "%.0f" % 0.5, "%.0f" % 1.5, "%.0f" % 2.5, "%.1f" % 0.05)
print("%e", "%e" % 12345.678, "%.2E" % 0.000123, "%g" % 0.0001, "%g" % 1e-5, "%G" % 1e20)
print("%f int", "%f" % 3, "%.3f" % 10**3)
print("%c", "%c" % 65, "%c" % "z", "%c" % 9731)
print("%%", "100%%" % (), "%d%%" % 50)
print("%-5s", "[%-5s]" % "ab", "[%5s]" % "ab", "[%.1s]" % "ab", "[%5.1s]" % "ab")
print("%s float", "%s" % 0.1, "%s" % 1e16, "%r" % 1.0)
print("%s special", "%s %f %e" % (float("inf"), float("-inf"), float("nan")))

# tuple operands
print("tuple", "%s and %s" % ("a", "b"))
print("tuple mixed", "%d-%s-%.1f" % (1, "two", 3.0))
print("tuple as single value", "%s" % ((1, 2),))
print("list is a single value", "%s" % [1, 2])

# * width and precision
print("star width", "[%*d]" % (6, 42), "[%-*d]" % (6, 42), "[%.*f]" % (2, 3.14159), "[%*.*f]" % (8, 3, 2.5))

# dict operands
print("dict", "%(name)s is %(age)d" % {"name": "Al", "age": 30})
print("dict reuse", "%(x)s %(x)r %(x)5s|" % {"x": "v"})
print("dict with spec", "%(v)08.3f" % {"v": 3.14159})
print("dict extra keys ok", "%(a)s" % {"a": 1, "b": 2})
print("dict %%", "%(a)s%%" % {"a": 5})

# objects with __str__/__repr__
class Obj:
    def __str__(self):
        return "Obj-str"

    def __repr__(self):
        return "Obj-repr"

print("objects", "%s %r" % (Obj(), Obj()))

# __index__ for %x and %d
class Idx:
    def __index__(self):
        return 26

try:
    print("index obj", "%x %d %o" % (Idx(), Idx(), Idx()))
except TypeError as e:
    print("index obj", type(e).__name__, e)

# errors
def attempt(label, fmt, args):
    try:
        print(label, repr(fmt % args))
    except Exception as e:
        print(label, type(e).__name__, e)

attempt("not enough args", "%s %s", ("a",))
attempt("too many args", "%s", ("a", "b"))
attempt("no args for format", "%d", ())
attempt("d with str", "%d", "x")
attempt("x with float", "%x", 1.5)
attempt("f with str", "%f", "1.0")
attempt("c with long str", "%c", "ab")
attempt("c out of range", "%c", 0x110000)
attempt("unsupported char", "%y", 1)
attempt("incomplete format", "%", ())
attempt("incomplete at end", "abc %", 1)
attempt("dict key missing", "%(missing)s", {"a": 1})
attempt("mapping required", "%(a)s", ("a",))
attempt("star with dict", "%(a)*d", {"a": 1})
attempt("star width not int", "%*d", ("5", 1))
attempt("str without specs and args", "plain", 1)
attempt("str without specs and empty tuple", "plain", ())
attempt("str without specs and dict", "plain", {"k": 1})
attempt("unmatched paren", "%(a", {"a": 1})

# bytes %-formatting
print("bytes", b"%s|%d|%x|%5.1f" % (b"raw", 10, 255, 1.25))
print("bytes %c", b"%c%c" % (65, b"z"), b"%%" % ())
print("bytes %a %r", b"%a %r" % ("é", "x"))
attempt("bytes %s with str", b"%s", "text")
attempt("bytes %c too big", b"%c", 256)

# percent with str subclass of tuple
class T(tuple):
    pass

print("tuple subclass", "%s-%s" % T((1, 2)))
