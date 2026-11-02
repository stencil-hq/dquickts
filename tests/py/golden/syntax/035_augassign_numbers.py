# Augmented assignment on numbers: every operator, int/float/bool/complex, errors.

x = 10
x += 5; print("+=", x)
x -= 3; print("-=", x)
x *= 4; print("*=", x)
x /= 8; print("/=", x, type(x).__name__)
x = 17
x //= 5; print("//=", x)
x %= 2; print("%=", x)
x = 3
x **= 4; print("**=", x)
x <<= 2; print("<<=", x)
x >>= 3; print(">>=", x)
x &= 0b1010; print("&=", x)
x |= 0b0101; print("|=", x)
x ^= 0b1111; print("^=", x)

# negative operands
y = -17
y //= 5; print("neg //=", y)
y = -17
y %= 5; print("neg %=", y)
y = 17
y %= -5; print("%= neg", y)
y = -8
y >>= 1; print("neg >>=", y)
y = -1
y <<= 70; print("neg <<= big", y)
y = -6
y &= 0xFF; print("neg &=", y)
y = 2
y **= -1; print("**= neg", y, type(y).__name__)
y = -8
y **= 1 / 3; print("neg **= fraction", type(y).__name__)

# floats
f = 1.5
f += 1; print("float +=", f)
f *= -2; print("float *=", f)
f //= 2; print("float //=", f)
f %= 2.5; print("float %=", f)
f **= 2; print("float **=", f)
f = 7.0
f /= 0.5; print("float /=", f)

# bool becomes int
b = True
b += True; print("bool +=", b, type(b).__name__)
b = True
b &= False; print("bool &=", b, type(b).__name__)
b = True
b ^= True; print("bool ^=", b, type(b).__name__)

# complex
z = 1 + 2j
z *= 1j; print("complex *=", z)
z += 1; print("complex +=", z)
z **= 2; print("complex **=", z)

# big ints
big = 2**64
big *= big; print("big *=", big)
big //= 10**10; print("big //=", big)
big >>= 100; print("big >>=", big)

# errors
def attempt(label, fn):
    try:
        print(label, repr(fn()))
    except Exception as e:
        print(label, type(e).__name__, e)

def aug(op, a, b):
    if op == "/=":
        a /= b
    elif op == "//=":
        a //= b
    elif op == "%=":
        a %= b
    elif op == "<<=":
        a <<= b
    elif op == "&=":
        a &= b
    elif op == "+=":
        a += b
    elif op == "**=":
        a **= b
    return a

attempt("int /= 0", lambda: aug("/=", 1, 0))
attempt("int //= 0", lambda: aug("//=", 1, 0))
attempt("int %= 0", lambda: aug("%=", 1, 0))
attempt("float /= 0", lambda: aug("/=", 1.0, 0.0))
attempt("float %= 0", lambda: aug("%=", 1.0, 0))
attempt("<<= negative", lambda: aug("<<=", 1, -1))
attempt("&= float", lambda: aug("&=", 1, 1.0))
attempt("+= None", lambda: aug("+=", 1, None))
attempt("+= str", lambda: aug("+=", 1, "s"))
attempt("0 **= -1", lambda: aug("**=", 0, -1))
attempt("int **= huge stays exact", lambda: aug("**=", 10, 40))
attempt("complex //=", lambda: aug("//=", 1j, 1))

# undefined name in augmented assignment
try:
    undefined_counter += 1
except NameError as e:
    print("undefined +=", type(e).__name__, e)

# augmented assignment on a local before assignment
def local_aug():
    count += 1

try:
    local_aug()
except UnboundLocalError as e:
    print("unbound local +=", type(e).__name__, e)

# augmented assignment is a statement, not an expression
for src in ["x = (y += 1)", "x += 1 += 2", "a, b += 1, 2", "[a] += [1]", "f() += 1", "x +== 1"]:
    try:
        compile(src, "<aug>", "exec")
        print("compiles", repr(src))
    except SyntaxError as e:
        print("syntax", repr(src), e.msg)
