# exec / eval / compile with explicit namespaces, __builtins__ handling, modes and errors.

g = {}
exec("x = 1\ny = x + 1", g)
print("exec globals", g["x"], g["y"], "__builtins__ injected", "__builtins__" in g)
print("exec user keys", sorted(k for k in g if not k.startswith("__")))

g = {"a": 10}
loc = {}
exec("b = a * 2\nc = b + 1", g, loc)
print("exec locals", sorted(loc.items()), "globals untouched", "b" in g)

print("eval", eval("1 + 2 * 3"))
print("eval with globals", eval("a + b", {"a": 1, "b": 2}))
print("eval with locals override", eval("a", {"a": "global"}, {"a": "local"}))
print("eval returns object", eval("[i * i for i in range(4)]"))
print("exec returns None", exec("pass"))

try:
    eval("len('abc')", {"__builtins__": {}})
except NameError as e:
    print("empty builtins", type(e).__name__, e)
print("custom builtins", eval("len('abc')", {"__builtins__": {"len": lambda s: "fake " + s}}))
print("builtins from dict", eval("abs(-3)", {"__builtins__": {"abs": abs}}))

try:
    eval("x = 1")
except SyntaxError as e:
    print("eval statement", type(e).__name__, e.msg)
try:
    eval("1 +")
except SyntaxError as e:
    print("eval incomplete", type(e).__name__, e.lineno)
try:
    exec("def f(:\n    pass")
except SyntaxError as e:
    print("exec syntax error", type(e).__name__, e.lineno)
try:
    exec("if True:\nprint(1)")
except IndentationError as e:
    print("indentation", type(e).__name__, e.lineno)
try:
    exec("raise KeyError('inside exec')", {})
except KeyError as e:
    print("runtime error in exec", type(e).__name__, e)

code = compile("result = n * 2", "<generated>", "exec")
print("code object", type(code).__name__, code.co_filename, code.co_name)
ns = {"n": 21}
exec(code, ns)
print("compiled exec", ns["result"])
ns["n"] = 5
exec(code, ns)
print("code reusable", ns["result"])

expr = compile("p ** 2", "<expr>", "eval")
print("compiled eval", [eval(expr, {"p": p}) for p in range(4)])

single = compile("40 + 2", "<single>", "single")
print("single mode prints:")
exec(single)
exec(compile("None", "<single>", "single"))
print("single None printed nothing")

try:
    compile("x", "<bad>", "bogus")
except ValueError as e:
    print(type(e).__name__, e)
try:
    eval(compile("y = 1", "<s>", "exec"))
except NameError as e:
    print("eval of exec code", type(e).__name__, e)
else:
    print("eval of exec code returns None")

src = "def greet(name):\n    return 'hi ' + name\n"
env = {}
exec(src, env)
greet = env["greet"]
print("defined via exec", greet("bob"), greet.__name__, greet.__globals__ is env)

print("exec class", end=" ")
exec("class K:\n    v = 7\nprint(K.v, K.__module__)", {"__name__": "fake_module"})


def outer():
    local_var = "local"
    return eval("local_var")


print("eval sees function locals", outer())


def exec_cannot_rebind():
    v = 1
    exec("v = 2")
    return v


print("exec cannot rebind fast locals", exec_cannot_rebind())
print("eval whitespace", eval("  1 + 1  "), eval("\n2\n"))
print("eval bytes source", eval(b"3 * 3"))
try:
    eval(5)
except TypeError as e:
    print(type(e).__name__, e)
try:
    exec("x", [])
except TypeError as e:
    print(type(e).__name__, e)
print("nested eval", eval("eval('1+1')"))
print("dont_inherit flag ok", eval(compile("1", "<c>", "eval", dont_inherit=True)))
