# Closures: late binding, default-argument capture trick, cell sharing, closure introspection.

funcs = []
for i in range(3):
    funcs.append(lambda: i)
print("late binding", [f() for f in funcs])

funcs = []
for i in range(3):
    funcs.append(lambda i=i: i)
print("default trick", [f() for f in funcs])

print("comprehension late", [f() for f in [lambda: j for j in range(3)]])
print("comprehension trick", [f() for f in [lambda j=j: j for j in range(3)]])


def make_adders():
    adders = []
    for n in range(1, 4):

        def add(x, n=n):
            return x + n

        adders.append(add)
    return adders


print("adders", [a(10) for a in make_adders()])


def factory(n):
    def mul(x):
        return x * n

    return mul


double = factory(2)
triple = factory(3)
print("factory", double(5), triple(5))
print("closure cells", len(double.__closure__))
print("cell contents", double.__closure__[0].cell_contents)
print("freevars", double.__code__.co_freevars)
print("no closure", factory.__closure__)


def shared_cell():
    value = 0

    def get():
        return value

    def set_(v):
        nonlocal value
        value = v

    return get, set_


get, set_ = shared_cell()
print("shared before", get())
set_(42)
print("shared after", get())


def rebind_after():
    def show():
        return x

    x = "late"
    return show


print("assigned after def", rebind_after()())


def unbound_free():
    def show():
        return y

    try:
        show()
    except NameError as e:
        print("free var unbound", type(e).__name__, e)
    y = 1
    return show()


print("then bound", unbound_free())


def nested_three():
    a = 1

    def level2():
        b = 2

        def level3():
            return a + b

        return level3

    return level2()()


print("three levels", nested_three())

x = "global"


def shadow():
    x = "enclosing"

    def inner():
        return x

    return inner()


print("enclosing wins", shadow())


def class_scope():
    v = "func"

    class C:
        v = "class"

        def m(self):
            return v

        w = [v for _ in range(1)]

    return C().m(), C.w


print("class scope skipped", class_scope())


def generator_closure():
    total = 10
    return (total + k for k in range(3))


print("genexp closure", list(generator_closure()))
print("closure eq", double.__closure__[0] == double.__closure__[0])
