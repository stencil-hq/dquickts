# if/elif/else chains, nested ifs, conditions with side effects.

def classify(n):
    if n < 0:
        return "negative"
    elif n == 0:
        return "zero"
    elif n < 10:
        return "small"
    elif n < 100:
        return "medium"
    else:
        return "large"

for v in [-5, 0, 3, 10, 99, 100, 10**20, -0.0, 0.5]:
    print("classify", repr(v), classify(v))

# only the first matching branch runs
x = 5
if x > 1:
    print("branch >1")
elif x > 2:
    print("branch >2 (never)")
else:
    print("else (never)")

# if without else
if x == 6:
    print("never")
print("after if without else")

# elif without else, nothing matching
if x == 1:
    print("never")
elif x == 2:
    print("never")
print("no branch taken")

# nested ifs
def grade(score, bonus):
    if score >= 50:
        if bonus:
            return "pass+"
        else:
            return "pass"
    else:
        if score + 10 >= 50 and bonus:
            return "rescued"
        return "fail"

print("grade", grade(70, True), grade(70, False), grade(45, True), grade(45, False))

# conditions are evaluated lazily in order
log = []
def t(name, value):
    log.append(name)
    return value

if t("a", False):
    pass
elif t("b", True):
    pass
elif t("c", True):
    pass
print("evaluated conditions", log)

log.clear()
if t("x", 0) or t("y", "") or t("z", [1]):
    print("or chain true")
print("or chain log", log)

log.clear()
if t("p", 1) and t("q", 0) and t("r", 1):
    print("never")
else:
    print("and chain false")
print("and chain log", log)

# conditions using various types
for cond in [[], [0], "", " ", {}, {0: 0}, None, 0, 1, 0.0, 1e-300]:
    if cond:
        print("truthy", repr(cond))
    else:
        print("falsy", repr(cond))

# single-line if bodies
if True: print("single line if")
if False: print("never")
else: print("single line else")

# comparison result used directly
y = 7
if y % 2 == 1 and y > 5:
    print("odd and > 5")

# condition raising an exception
class Bad:
    def __bool__(self):
        raise ValueError("no bool for you")

try:
    if Bad():
        pass
except ValueError as e:
    print("bool raises", type(e).__name__, e)

# __bool__ returning a non-bool
class WrongBool:
    def __bool__(self):
        return 1

try:
    if WrongBool():
        pass
except TypeError as e:
    print("bool returns int", type(e).__name__, e)

# elif chains on strings
def kind(s):
    if not s:
        return "empty"
    elif s.isdigit():
        return "digits"
    elif s.isalpha():
        return "alpha"
    elif s.isspace():
        return "space"
    return "mixed"

for s in ["", "123", "abc", "  ", "a1"]:
    print("kind", repr(s), kind(s))

# if with assignment in body affects outer scope (no block scope)
if True:
    inner = "visible"
print("block scope", inner)
