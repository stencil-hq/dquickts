# Conditional expressions, and/or returning operands, not, short-circuiting.

print("ternary true", "yes" if 1 else "no")
print("ternary false", "yes" if 0 else "no")
print("ternary nested", "neg" if -1 < 0 else "zero" if 0 == 0 else "pos")
for n in [-3, 0, 4]:
    print("sign", n, "neg" if n < 0 else "zero" if n == 0 else "pos")

# right associativity of nested ternary
v = 1 if False else 2 if False else 3
print("nested ternary assoc", v)

# ternary with side effects: only one branch evaluated
log = []
def f(name, value):
    log.append(name)
    return value

r = f("a", 1) if f("cond", True) else f("b", 2)
print("ternary lazy", r, log)

# and/or return one of their operands
print("or first truthy", repr(0 or "x"))
print("or all falsy", repr(0 or "" or None))
print("or last falsy", repr(None or []))
print("or first", repr([1] or 0))
print("and first falsy", repr(1 and 0 and 2))
print("and all truthy", repr(1 and "a" and [3]))
print("and empty str", repr("" and 1))
print("and/or mix", repr(0 and 1 or 2))
print("or/and precedence", repr(1 or 0 and 0))
print("and binds tighter", repr(False or True and False))
print("default idiom", repr(None or "default"))
print("guard idiom", repr([] and [][0]))
print("float zero or", repr(0.0 or -0.0))
print("neg zero and", repr(-0.0 and 1))

# short-circuit side effects
log.clear()
r = f("x", 0) and f("y", 1)
print("and short-circuit", repr(r), log)
log.clear()
r = f("x", 5) or f("y", 1)
print("or short-circuit", repr(r), log)
log.clear()
r = f("x", 0) or f("y", 0) or f("z", 0)
print("or all evaluated", repr(r), log)

# not always returns a bool
print("not 0", not 0)
print("not 1", not 1)
print("not empty str", not "")
print("not list", not [0])
print("not None", not None)
print("not not str", not not "abc")
print("type not", type(not 5).__name__)
print("not precedence", not 1 == 2)
print("not with and", not 0 and 0)
print("not with or", not (0 or 0))

# and/or with objects defining __bool__
class B:
    def __init__(self, v, name):
        self.v = v
        self.name = name

    def __bool__(self):
        print("  __bool__", self.name)
        return self.v

    def __repr__(self):
        return "B(" + self.name + ")"

print("or returns object", B(False, "p") or B(True, "q"))
print("and returns object", B(True, "p") and B(False, "q"))
print("not object", not B(True, "n"))

# bool() of operands
print("bool and", bool(1 and 2), bool(1 and 0))

# ternary used in function arguments and comprehensions
print("ternary in call", max(3 if True else 4, 1))
print("ternary in comp", ["even" if i % 2 == 0 else "odd" for i in range(4)])

# ternary with lambda
pick = (lambda a, b: a) if True else (lambda a, b: b)
print("ternary lambdas", pick("L", "R"))

# comparison chains combined with and/or
x = 5
print("chain or", x < 0 or 3 < x < 10)
print("chain and", x > 0 and x < 3)

# and/or result types
for expr in [1 and 2.5, 0 or 2.5, "" or b"", [] and {}, () or None]:
    print("result type", type(expr).__name__, repr(expr))

# conditional expression in a return statement
def absval(n):
    return n if n >= 0 else -n

print("absval", absval(-7), absval(7), absval(0))

# conditional expression as default argument
def greet(name=None):
    return "Hello, " + (name if name else "world")

print("greet", greet(), greet("Ann"), greet(""))
