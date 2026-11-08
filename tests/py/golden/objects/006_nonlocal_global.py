# nonlocal and global: counters, rebinding, UnboundLocalError cases.


def make_counter(start=0):
    count = start

    def incr(step=1):
        nonlocal count
        count += step
        return count

    return incr


c1 = make_counter()
c2 = make_counter(100)
print("counter 1", c1(), c1(), c1(5))
print("counter 2", c2(), c2())
print("independent", c1(), c2())


def bad():
    n = 0

    def inc():
        n += 1
        return n

    return inc


try:
    bad()()
except UnboundLocalError as e:
    print(type(e).__name__, e)

print("UnboundLocalError is NameError", issubclass(UnboundLocalError, NameError))

g_value = 1


def read_then_assign():
    try:
        print(g_value)
    except UnboundLocalError as e:
        print("read then assign", type(e).__name__, e)
    g_value = 2
    return g_value


print("local result", read_then_assign(), "global still", g_value)


def set_global():
    global g_value
    g_value = 50


set_global()
print("global set", g_value)


def deleted_local():
    v = 1
    del v
    try:
        return v
    except UnboundLocalError as e:
        return type(e).__name__ + ": " + str(e)


print("deleted local", deleted_local())


def outer():
    x = "outer"

    def middle():
        nonlocal x
        x = "middle"

        def inner():
            nonlocal x
            x = "inner"

        inner()
        return x

    r = middle()
    return r, x


print("nonlocal chain", outer())


def nonlocal_skip_global():
    v = "enclosing"

    def inner():
        global v_glob
        v_glob = "g"
        return v

    return inner()


print("mixed scopes", nonlocal_skip_global(), v_glob)


def loop_counter():
    hits = 0

    def hit():
        nonlocal hits
        hits += 1

    for _ in range(7):
        hit()
    return hits


print("loop counter", loop_counter())


def conditional_assign(flag):
    if flag:
        z = "set"
    return z


print("conditional ok", conditional_assign(True))
try:
    conditional_assign(False)
except UnboundLocalError as e:
    print(type(e).__name__, e)

try:
    print(undefined_name_xyz)
except NameError as e:
    print(type(e).__name__, e)
