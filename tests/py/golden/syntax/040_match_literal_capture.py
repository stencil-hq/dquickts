# match statement: literal, capture, wildcard, or-patterns, as, guards, value patterns.

def lit(x):
    match x:
        case 0:
            return "zero"
        case 1 | 2 | 3:
            return "small"
        case -1:
            return "minus one"
        case 1.5:
            return "one and a half"
        case 2j:
            return "imaginary"
        case 1 + 2j:
            return "complex"
        case "hello":
            return "greeting"
        case "a" | "b":
            return "a or b"
        case b"raw":
            return "bytes"
        case None:
            return "none"
        case True:
            return "true"
        case False:
            return "false"
        case _:
            return "other"

for v in [0, 1, 3, -1, 1.5, 2j, 1 + 2j, "hello", "b", b"raw", None, True, False, 0.0, 1.0,
          "x", [], 4]:
    print("literal", repr(v), lit(v))

# True/False/None match by identity, numbers by equality
print("1 vs True", lit(1), "True vs 1", lit(True), "0.0 vs 0", lit(0.0))

# capture pattern binds the subject
def cap(x):
    match x:
        case 0:
            return "zero"
        case other:
            return "captured " + repr(other)

print("capture", cap(0), cap("anything"), cap([1]))

# wildcard does not bind
match 99:
    case _:
        print("wildcard matched")
try:
    print(_)
except NameError as e:
    print("wildcard no bind", type(e).__name__)

# or-pattern with captures must bind same names
def orcap(x):
    match x:
        case (1, y) | (2, y):
            return "pair y=" + repr(y)
        case [y] | (y, _, _):
            return "single-or-triple y=" + repr(y)
    return "no match"

print("or capture", orcap((1, "a")), orcap([2, "b"]), orcap(["s"]), orcap((7, 8, 9)), orcap(()))

# as patterns
def aspat(x):
    match x:
        case (0 | 1) as bit:
            return "bit " + repr(bit)
        case [1, 2] as whole:
            return "whole " + repr(whole)
        case str() as s:
            return "str " + s
    return "none"

print("as", aspat(0), aspat(1), aspat([1, 2]), aspat((1, 2)), aspat("s"), aspat(5))

# guards
def guard(x):
    match x:
        case int(n) if n < 0:
            return "negative"
        case int(n) if n % 2 == 0:
            return "even"
        case int():
            return "odd"
        case _ if isinstance(x, str) and x:
            return "nonempty str"
    return "other"

print("guards", guard(-3), guard(4), guard(7), guard("q"), guard(""), guard(1.0))

# guard is evaluated after binding, and a failing guard tries the next case
log = []
def g(v):
    log.append(v)
    return False

match 5:
    case x if g(x):
        print("never")
    case y:
        print("fallthrough after guard", x, y, log)

# value patterns use dotted names
class Color:
    RED = "red"
    GREEN = "green"

class Codes:
    class Http:
        OK = 200
        NOT_FOUND = 404

def val(x):
    match x:
        case Color.RED:
            return "is red"
        case Color.GREEN | Codes.Http.OK:
            return "green or ok"
        case Codes.Http.NOT_FOUND:
            return "not found"
    return "unknown"

print("value patterns", val("red"), val("green"), val(200), val(404), val(500))

# subject expression evaluated once
cnt = []
def subject():
    cnt.append(1)
    return 3

match subject():
    case 1:
        pass
    case 2:
        pass
    case 3:
        print("subject once", len(cnt))

# match and case are soft keywords
match = "still a name"
case = 1
print("soft keywords", match, case)
