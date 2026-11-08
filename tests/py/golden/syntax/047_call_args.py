# Function call basics: positional/keyword args, defaults, *args, **kwargs, kw-only, pos-only.

def f(a, b=2, *args, c=3, d, **kwargs):
    return "a=%r b=%r args=%r c=%r d=%r kwargs=%r" % (a, b, args, c, d, kwargs)

print("minimal", f(1, d=4))
print("positional b", f(1, 20, d=4))
print("extra positional", f(1, 2, 3, 4, d=5))
print("keyword everything", f(d=4, c=30, b=20, a=10))
print("extra keywords", f(1, d=4, x=1, y=2))
print("keyword order kept", f(1, d=0, zz=1, aa=2))

def g(x, y, /, z, *, w):
    return (x, y, z, w)

print("pos-only", g(1, 2, 3, w=4), g(1, 2, z=3, w=4))

def show(*args, **kwargs):
    return args, kwargs

print("empty", show())
print("kwargs named like params", show(args=1, kwargs=2, self=3))

# pos-only parameter names can be reused as **kwargs keys
def po(a, /, **kw):
    return a, kw

print("pos-only name in kwargs", po(1, a=2))

# defaults are evaluated once at def time
def mutable_default(item, bucket=[]):
    bucket.append(item)
    return bucket

print("mutable default", mutable_default(1), mutable_default(2))

def safe_default(item, bucket=None):
    if bucket is None:
        bucket = []
    bucket.append(item)
    return bucket

print("None default idiom", safe_default(1), safe_default(2))

base = 10
def captured_default(x=base):
    return x

base = 20
print("default captured at def", captured_default())
print("defaults tuple", captured_default.__defaults__, f.__kwdefaults__)

# errors
def attempt(label, fn):
    try:
        print(label, fn())
    except TypeError as e:
        print(label, type(e).__name__, e)

attempt("missing required kw-only", lambda: f(1))
attempt("missing positional", lambda: f(d=1))
attempt("multiple values", lambda: f(1, a=2, d=3))
attempt("pos-only passed as kw", lambda: g(1, y=2, z=3, w=4))
attempt("kw-only passed positionally", lambda: g(1, 2, 3, 4))
attempt("too many positional", lambda: g(1, 2, 3, 4, w=5))
attempt("unexpected kw", lambda: g(1, 2, 3, w=4, q=5))
attempt("two missing", lambda: (lambda a, b, c: 0)(1))
attempt("three missing kw-only", lambda: (lambda *, a, b, c: 0)())
attempt("call non-callable", lambda: 5())
attempt("call None", lambda: None(1))
attempt("call str", lambda: "s"(1))

def zero():
    return "zero"

attempt("args to zero-arg", lambda: zero(1))
attempt("kwargs to zero-arg", lambda: zero(x=1))

# keyword argument syntax errors (compile only)
for src in ["f(a=1, 2)", "f(a=1, a=2)", "f(**k, *a)", "f(1=2)", "def h(a=1, b): pass",
            "def h(a, a): pass", "def h(*, **k): pass", "def h(**k, a): pass", "f(a for a in b, c)",
            "def h(a, /, b, /): pass", "def h(/, a): pass"]:
    try:
        compile(src, "<c>", "exec")
        print("compiles", repr(src))
    except SyntaxError as e:
        print("syntax", repr(src), e.msg)

# return values: implicit None, multiple values, early return
def nothing():
    pass

def multi():
    return 1, "two", [3]

def early(x):
    if x:
        return "early"
    return "late"

print("returns", nothing(), multi(), early(1), early(0))

# keyword-only arguments after *args
def joiner(*parts, sep="-"):
    return sep.join(parts)

print("kw after varargs", joiner("a", "b"), joiner("a", "b", sep="+"), joiner())

# recursion with default accumulator
def total(nums, acc=0):
    if not nums:
        return acc
    return total(nums[1:], acc + nums[0])

print("recursive default", total([1, 2, 3, 4]))

# argument evaluation happens before the call, left to right
order = []
def arg(v):
    order.append(v)
    return v

show(arg(1), arg(2), k=arg(3), *[arg(4)])
print("arg order", order)
