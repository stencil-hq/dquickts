# global and nonlocal declarations, closures, UnboundLocalError.

counter = 0

def increment():
    global counter
    counter += 1
    return counter

print("global inc", increment(), increment(), counter)

def read_global():
    return counter * 10

print("read global without decl", read_global())

def shadow():
    counter = "local"
    return counter

print("local shadows", shadow(), counter)

def create_global():
    global brand_new
    brand_new = "created inside function"

create_global()
print("created global", brand_new)

# UnboundLocalError: assignment anywhere makes the name local
x = "global x"

def broken():
    print(x)
    x = "local x"

try:
    broken()
except UnboundLocalError as e:
    print("unbound local", type(e).__name__, e)

def conditional_assign(flag):
    if flag:
        y = 1
    return y

print("conditional assigned", conditional_assign(True))
try:
    conditional_assign(False)
except UnboundLocalError as e:
    print("conditional unbound", type(e).__name__, e)

# nonlocal
def outer():
    n = 0
    def inner():
        nonlocal n
        n += 1
        return n
    inner()
    inner()
    return n

print("nonlocal", outer())

def make_accumulator():
    total = 0
    items = []
    def add(v):
        nonlocal total
        total += v
        items.append(v)
        return total, list(items)
    return add

acc = make_accumulator()
print("accumulator", acc(5), acc(10))
acc2 = make_accumulator()
print("independent closures", acc2(1))

# nonlocal reaches the nearest enclosing function scope
def level1():
    v = "L1"
    def level2():
        v = "L2"
        def level3():
            nonlocal v
            v = "L3 changed L2"
        level3()
        return v
    r = level2()
    return v, r

print("nearest scope", level1())

# closures see later assignments in the enclosing scope (late binding)
def late():
    def get():
        return value
    value = "assigned after def"
    return get()

print("late binding", late())

# closure over a loop variable
def closures():
    fs = []
    for i in range(3):
        def f():
            return i
        fs.append(f)
    return [f() for f in fs]

print("loop closures", closures())

# global inside nested function refers to module scope, not enclosing
g = "module g"
def enclosing():
    g = "enclosing g"
    def inner():
        global g
        return g
    return inner()

print("global skips enclosing", enclosing())

# syntax errors (compile only)
for src in ["def f():\n    nonlocal q\n",
            "nonlocal z\n",
            "def f(a):\n    global a\n",
            "def f():\n    x = 1\n    global x\n",
            "def f():\n    print(x)\n    global x\n",
            "def o():\n    a = 1\n    def i():\n        global a\n        nonlocal a\n"]:
    try:
        compile(src, "<g>", "exec")
        print("compiles", repr(src))
    except SyntaxError as e:
        print("syntax", repr(src), e.msg)

# del of a global from a function
def remove():
    global brand_new
    del brand_new

remove()
try:
    brand_new
except NameError as e:
    print("deleted global", type(e).__name__, e)
