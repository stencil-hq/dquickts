# Python side of the JS <-> Python interop tests (plan D3); run with
#   ./qpy tests/py/interop/test_interop.py
# Prints "PASS test_interop.py" and exits 0; a failure raises (exit 1).
import js
import gc
import _weakref

checks = 0


def check(cond, msg):
    global checks
    checks += 1
    if not cond:
        raise AssertionError(msg)


def raises(exc_type, f, *args):
    try:
        f(*args)
    except exc_type as e:
        return e
    raise AssertionError(f"{f!r} did not raise {exc_type.__name__}")


def test_js_module():
    check(js.globalThis.Math is js.Math, "module attributes are globalThis properties")
    check(js.Math.max(1, 5) == 5, "calling a JS function")
    check(isinstance(js.Object, js.JsObject), "JsObject type")
    check(issubclass(js.JsError, Exception), "JsError(Exception)")
    raises(AttributeError, getattr, js, "no_such_global_name")
    from js import JSON
    check(JSON is js.JSON, "from js import name")


def test_jsobject():
    o = js.JSON.parse('{"a": 1, "b": [1, 2, {"c": null}], "n": null}')
    check(type(o) is js.JsObject, "type(JS object)")
    check(repr(o) == "<JsObject [object Object]>", "repr")
    check(str(o) == "[object Object]", "str")
    check(o.a == 1 and o["a"] == 1, "attribute and item access")
    check("a" in o and "zz" not in o, "in")
    check(o.n is None, "null reads as None")
    check(type(o.b) is list and o.b[2].c is None, "JS arrays are lists")
    raises(AttributeError, getattr, o, "zz")
    raises(KeyError, lambda: o["zz"])
    o.z = 5
    check(list(js.Object.keys(o)) == ["a", "b", "n", "z"], "setattr = [[Set]]")
    del o.z
    check("z" not in o, "delattr")
    check(o == o and o != js.JSON.parse("{}"), "== is identity")
    check(hash(o) == hash(o), "hashable")
    check(bool(o), "objects are true")
    # methods get their `this`
    g = js.eval("({ x: 7, getX() { return this.x; } })")
    check(g.getX() == 7, "method call binds this")
    # constructors: .new()
    m = js.Map.new()
    m.set("k", 1)
    check(m["k"] == 1 and len(m) == 1 and "k" in m, "Map as a mapping")
    check(list(m) == ["k"], "Map iterates keys")
    m["j"] = 2
    check(m.get("j") == 2, "Map __setitem__")
    del m["j"]
    check(len(m) == 1, "Map __delitem__")
    check(dict(m) == {"k": 1}, "dict(Map)")
    check(m.to_py() == {"k": 1}, "Map.to_py()")
    s = js.Set.new([1, 2])
    check(2 in s and len(s) == 2 and sorted(s) == [1, 2], "Set")
    c = js.import_module("./jsmod.mjs").Counter.new(5)
    check(c.inc() == 6 and c.n == 6, "class instance from Python")
    # deep conversion
    d = js.JSON.parse('{"x": [1, {"y": 2}], "z": "s"}').to_py()
    check(d == {"x": [1, {"y": 2}], "z": "s"} and type(d["x"][1]) is dict, "to_py is deep")
    shallow = js.JSON.parse('{"x": {"y": 2}}').to_py(depth=1)
    check(type(shallow["x"]) is js.JsObject, "to_py depth")
    jo = js.to_js({"a": (1, 2), "b": {3}, "c": [b"\x01"]})
    check(js.Array.isArray(jo.a) and jo.a[1] == 2, "to_js: tuple -> Array")
    check(jo.b.has(3), "to_js: set -> Set")
    check(js.String(jo.c[0]) == "1", "to_js: bytes -> Uint8Array")
    # iteration over JS iterables
    gen = js.eval("(function* () { yield 1; yield 2; })")()
    check(list(gen) == [1, 2], "JS generator iteration")


def test_identity_and_calls():
    f = js.eval("(a) => a")
    lst = [1, 2]
    check(f(lst) is lst, "list identity")
    s = "h\u00e9llo \U0001f600"
    check(f(s) == s, "str identity")
    check(f(None) is None, "None round trip")
    js.eval("(a, x) => a.push(x)")(lst, 3)
    check(lst == [1, 2, 3], "JS mutation of a Python list")
    # keywords reach JS as one trailing object
    kwf = js.eval("(a, o) => [a, o.x, o.y]")
    check(kwf(1, x=2, y=3) == [1, 2, 3], "keywords -> trailing object")
    # Python callables called by JS; callKw
    def pyf(a, b=2, *, c=3):
        return [a, b, c]
    call = js.eval("(f) => f(1)")
    check(call(pyf) == [1, 2, 3], "JS calls a Python function")
    callkw = js.eval("(f) => f.callKw([1], {c: 9})")
    check(callkw(pyf) == [1, 2, 9], "callKw")
    trailing = js.eval("(f) => f(1, {c: 9})")
    check(trailing(pyf)[1].c == 9, "a trailing object is positional")
    check(js.eval("(f) => typeof f")(len) == "function", "builtins are functions for JS")
    check(js.run(js.eval("(f) => Promise.resolve(-2).then(f)")(abs)) == 2,
          "builtin as JS callback")


def test_numbers():
    typeof = js.eval("(x) => typeof x")
    check(typeof(5) == "number", "int32")
    check(typeof(2**40) == "number", "int within 2**53 -> Number")
    check(typeof(-(2**53)) == "number", "-2**53 -> Number")
    check(typeof(2**53 + 1) == "bigint", "beyond 2**53 -> BigInt")
    check(typeof(2**60) == "bigint", "big int -> BigInt")
    check(typeof(1.5) == "number", "float")
    r = js.eval("() => 2**41")()
    check(type(r) is int and r == 2**41, "integral float64 result -> int")
    check(type(js.eval("() => 4.0")()) is int, "4.0 -> int")
    check(type(js.eval("() => 0.5")()) is float, "non-integral stays float")
    check(type(js.eval("() => NaN")()) is float, "NaN stays float")
    arr = js.eval("[2**41]")
    check(type(arr[0]) is float, "values inside containers are not converted")


def test_exceptions():
    err = raises(js.JsError, js.eval("() => { throw new TypeError('bad'); }"))
    check(js.String(err.js_error) == "TypeError: bad", "js_error holds the thrown value")
    check(str(err) == "TypeError: bad", "str(JsError)")
    thrown = js.Error.new("orig")
    err2 = raises(js.JsError, js.eval("(e) => { throw e; }"), thrown)
    check(err2.js_error is thrown, "identity of the thrown JS value")

    class MyErr(Exception):
        pass
    exc = MyErr("x")

    def thrower():
        raise exc
    catcher = js.eval("(f) => { try { f(); } catch (e) { return e; } }")
    check(catcher(thrower) is exc, "Python exception reaches JS unchanged")
    rethrow = js.eval("(f) => f()")
    e = raises(MyErr, rethrow, thrower)
    check(e is exc, "identity on the Python -> JS -> Python round trip")
    # a JsError escaping to JS is still the Python exception object
    def reraise_js():
        js.eval("(() => { throw thrown_value; })")()
    js.globalThis.thrown_value = thrown
    got = catcher(reraise_js)
    check(type(got) is js.JsError and got.js_error is thrown, "JsError through JS")


def test_async():
    async def main():
        v = await js.Promise.resolve(41)
        w = await js.eval("new Promise((r) => Promise.resolve().then(() => r(1)))")
        try:
            await js.Promise.reject(js.Error.new("no"))
        except js.JsError as e:
            check(e.js_error.message == "no", "rejection -> JsError")
        else:
            check(False, "rejected promise must raise")
        return v + w
    check(js.run(main()) == 42, "await Promise from Python")

    async def double(x):
        return x * 2
    jsawait = js.eval("async (c) => (await c) + 1")
    check(js.run(jsawait(double(5))) == 11, "Python coroutine awaited by JS")
    t = js.create_task(double(4))
    check(js.run(t) == 8, "create_task")

    async def fail():
        raise ValueError("async")
    raises(ValueError, js.run, fail())

    tla = js.import_module("./jsmod_tla.mjs")
    check(type(tla) is js.JsObject and js.run(tla).value == "tla", "import_module with TLA")


def test_import_module():
    mod = js.import_module("./jsmod.mjs")
    check(mod.answer == 42 and mod.twice(4) == 8, "js.import_module")
    check(js.import_module("./jsmod.mjs") is mod, "modules are cached")


def test_cycle_collected():
    called = []

    class Box:
        pass

    def make():
        b = Box()
        o = js.Object.new()
        o.box = b          # JS -> Python
        b.o = o            # Python -> JS
        return _weakref.ref(b, lambda r: called.append(1))
    wr = make()
    gc.collect()
    check(wr() is None, "JS <-> Python cycle collected")
    for _ in range(100000):
        if called:
            break
    check(called == [1], "weakref callback ran")


test_js_module()
test_jsobject()
test_identity_and_calls()
test_numbers()
test_exceptions()
test_async()
test_import_module()
test_cycle_collected()
print(f"PASS test_interop.py ({checks} checks)")
