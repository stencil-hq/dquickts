# globals() / locals() basics (filtered, sorted keys), namespace mutation, and scope rules
# visible through them.


def user_keys(ns):
    return sorted(k for k in ns if k.isidentifier() and not k.startswith("__"))


print("module dunders present", sorted(k for k in ("__name__", "__builtins__", "__doc__") if k in globals()))
print("__name__", __name__)
print("module globals start", user_keys(globals()))
alpha = 1
beta = "two"
print("after assignments", user_keys(globals()))
print("globals is locals at module", globals() is locals())

globals()["injected"] = "via globals()"
print("injected global", injected)
del globals()["injected"]
print("removed", "injected" in globals())


def show_locals(a, b=2, *args, **kw):
    c = a + b
    snapshot = locals()
    return user_keys(snapshot), snapshot["c"]


print("function locals", show_locals(1, 2, 3, k=4))


def locals_snapshot():
    x = 1
    snap = locals()
    x = 2
    snap2 = locals()
    return snap["x"], snap2["x"], snap is snap2


print("locals snapshots", locals_snapshot())


def locals_write():
    x = 1
    locals()["x"] = 99
    return x


print("writing locals() no effect", locals_write())


def closure_locals():
    outer_var = "outer"

    def inner():
        inner_var = "inner"
        return user_keys(locals()) + [outer_var]

    return inner()


print("free vars appear in locals", closure_locals())


def globals_from_function():
    return globals() is globals(), "alpha" in globals(), "show_locals" in globals()


print("globals in function", globals_from_function())


def set_global_via_dict():
    globals()["created_in_function"] = 123


set_global_via_dict()
print("created via dict", created_in_function)


class Body:
    a = 1
    b = a + 1
    keys = user_keys(locals())


print("class body locals", Body.keys, Body.b)

print("comprehension scope", [user_keys(locals()) for _ in range(1)])
gen_locals = list(user_keys(locals()) for _ in range(1))
print("genexp scope", gen_locals)


def lambda_locals():
    return (lambda p, q=1: user_keys(locals()))(0)


print("lambda locals", lambda_locals())
print("builtins not in globals", "len" in globals(), "print" in globals())
len_shadow_check = globals().get("len", "not shadowed")
print("get missing", len_shadow_check)
len = lambda x: "shadowed len"
print("shadowed builtin", len([1, 2]))
del len
print("restored builtin", len([1, 2]))


def uses_global_counter():
    global alpha
    alpha += 1
    return globals()["alpha"]


print("global keyword updates dict", uses_global_counter(), alpha)
print("vars() no arg is locals", vars() is locals())
for_loop_var_check = [k for k in ("i", "_") if k in globals()]
for i in range(2):
    pass
print("loop var leaks to module", "i" in globals(), i, for_loop_var_check)
print("final user globals", user_keys(globals()))
