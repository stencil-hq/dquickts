// JS side of the JS <-> Python interop tests (plan D3); run with
//   ./qjs -m tests/py/interop/test_interop.js
// Prints "PASS test_interop.js" and exits 0, or reports the first failure
// and exits 1.
import * as std from "std";
import * as py from "./pymod.py";
import pymod from "./pymod.py";

let checks = 0;
function check(cond, msg) {
    checks++;
    if (!cond)
        throw new Error("check failed: " + msg);
}
function eq(a, b, msg) {
    check(a === b, msg + ": got " + String(a) + ", expected " + String(b));
}
function sameJSON(a, b, msg) {
    eq(JSON.stringify(a), JSON.stringify(b), msg);
}
async function flushJobs(n = 10) {
    for (let i = 0; i < n; i++)
        await null;
}

function testModules() {
    // default export = the module object, named exports = public globals
    eq(pymod.add, py.add, "default export is the module object");
    eq(py.type_name(pymod), "module", "type of default export");
    check(!("_private" in py), "private globals are not exported");
    eq(pymod.__name__, "pymod", "module __name__");
    eq(py.type_name(py.js), "module", "`import js` inside the module");
}

function testIdentity() {
    const arr = [1, 2];
    eq(py.ident(arr), arr, "Array identity across the boundary");
    check(Array.isArray(py.items), "Python list is a JS Array");
    py.items.push(4);
    check(py.is_same(py.items, pymod.items), "list identity");
    eq(pymod.items.length, 4, "mutation visible from Python");
    const s = "h\u00e9llo \ud83d\ude00";
    eq(py.ident(s), s, "string identity");
    eq(py.ident(undefined), undefined, "None is undefined");
    eq(py.is_same(null, undefined), true, "null reads as None in Python");
    const obj = {};
    eq(py.ident(obj), obj, "object identity");
    eq(py.ident(py.ident), py.ident, "function identity");
}

function testCalls() {
    eq(py.add(2, 3), 5, "positional call");
    eq(py.add("a", "b"), "ab", "str concatenation");
    // a trailing plain object is a positional argument, not **kwargs
    const o = { c: 9 };
    const r = py.kw(1, o);
    eq(r[1], o, "trailing object is positional (b)");
    eq(r[2], 3, "c keeps its default");
    sameJSON(py.kw.callKw([1], { c: 9 }), [1, 2, 9], "callKw with kwargs");
    sameJSON(py.kw.callKw([1, 5]), [1, 5, 3], "callKw without kwargs");
    sameJSON(py.kw.callKw([], { a: 0, b: 1, c: 2 }), [0, 1, 2], "callKw keyword-only");
    let threw = false;
    try { py.add(1); } catch (e) { threw = py.type_name(e) === "TypeError"; }
    check(threw, "missing argument raises TypeError");
    eq(typeof py.add, "function", "typeof Python function");
    eq(typeof py.Point, "function", "typeof Python class");
    sameJSON(py.kw_to_js((a, kw) => [a, kw.x, kw.y]), [1, 2, 3],
             "Python keywords reach JS as a trailing object");
}

function testNumbers() {
    sameJSON(py.typeof_args((x) => typeof x),
             ["number", "number", "number", "bigint", "number"],
             "Python -> JS argument numbers");
    eq(py.ret_type(() => 2 ** 41), "int", "integral float64 result becomes int");
    eq(py.ret_type(() => 4.0), "int", "4.0 result becomes int");
    eq(py.ret_type(() => 0.5), "float", "non-integral result stays float");
    eq(py.type_name(2 ** 41), "float", "JS -> Python arguments are not converted");
    eq(typeof py.call_js(() => 2 ** 60), "bigint", "large integral result becomes int (BigInt in JS)");
}

function testJsObjectFromPython() {
    const o = { x: 5, getX() { return this.x; } };
    const info = py.jsobj_info(o);
    eq(info[0], "JsObject", "type of a JS object");
    eq(info[1], "<JsObject [object Object]>", "repr");
    eq(info[2], 5, "attribute = [[Get]]");
    eq(info[3], 5, "item access");
    eq(info[4], true, "in");
    eq(info[5], false, "not in");
    check(py.missing_attr(o), "missing property raises AttributeError");
    eq(py.call_method(o), 5, "methods are called with this");
    py.set_attr(o, "y", 7);
    eq(o.y, 7, "setattr = [[Set]]");
    const m = new Map([["a", 1], ["b", 2]]);
    const mi = py.map_info(m);
    eq(mi[0], 1, "Map item");
    eq(mi[1], 2, "len(Map)");
    eq(mi[2], true, "key in Map");
    eq(mi[3], false, "key not in Map");
    sameJSON(mi[4], ["a", "b"], "Map iterates its keys");
    eq(mi[5].get("b"), 2, "Map.to_py() is a dict");
    const d = py.deep({ a: [1, { b: 2 }], m: new Map([["k", 1]]), s: new Set([3]) });
    eq(py.type_name(d), "dict", "to_py: object -> dict");
    eq(d.get("a")[1].get("b"), 2, "to_py is deep");
    eq(d.get("m").get("k"), 1, "to_py: Map -> dict");
    eq(py.type_name(d.get("s")), "set", "to_py: Set -> set");
}

function testPythonObjectsFromJs() {
    const p = py.Point(3, -4);
    eq(p.x, 3, "instance attribute");
    eq(p.norm1(), 7, "bound method call");
    eq(String(p), "Point(3, -4)", "toString -> str()");
    eq(`${p}`, "Point(3, -4)", "template literal");
    eq(p.nothere, undefined, "AttributeError -> undefined");
    check(p instanceof py.Point, "instanceof Python class");
    check(!(p instanceof py.Box), "not instanceof another class");
    p.z = 10;
    eq(p.z, 10, "JS assignment sets a Python attribute");
    const d = py.d;
    eq(d.a, 1, "dict key through property access");
    eq(d.get("a"), 1, "dict.get from JS");
    eq(d.keys !== undefined, true, "dict methods win over keys");
    sameJSON(Object.keys(d), ["a", "b"], "Object.keys(dict)");
    eq(JSON.stringify(d), '{"a":1,"b":[1,2]}', "JSON.stringify(dict)");
    d.c = 3;
    eq(d.get("c"), 3, "assignment stores a dict item");
    check("c" in d, "in on dict");
    delete d.c;
    check(!("c" in d), "delete removes the dict item");
    const t = py.t;
    eq(t[1], "two", "tuple index");
    eq(t.length, 3, "tuple length");
    sameJSON([...t], [1, "two", null], "tuple iteration");
    eq(JSON.stringify(t), '[1,"two",null]', "JSON.stringify(tuple)");
    sameJSON([...py.s], [7], "set iteration");
    eq(JSON.stringify(py.s), "[7]", "JSON.stringify(set)");
    eq(pymod.items.map((x) => x * 2).length, 4, "a Python list has the Array methods");
}

function testExceptions() {
    // JS throw caught in Python: js.JsError holding the value
    const err = new Error("js boom");
    const e = py.catch_js(() => { throw err; });
    eq(py.type_name(e), "JsError", "JsError type");
    eq(e.js_error, err, "JsError.js_error is the thrown value");
    const e2 = py.catch_js(() => { throw 42; });
    eq(e2.js_error, 42, "non-Error throw values are kept");
    // Python exception escaping into JS: unchanged
    let caught;
    try { py.raise_value_error(); } catch (x) { caught = x; }
    eq(py.type_name(caught), "ValueError", "Python exception reaches JS unchanged");
    eq(String(caught), "from python", "str() of the exception");
    try { py.raise_stored(); } catch (x) { caught = x; }
    check(py.is_same(caught, py.stored), "identity: Python -> JS");
    const back = py.catch_any(() => { throw caught; });
    check(py.is_same(back, py.stored), "identity: Python -> JS -> Python");
}

async function testAsync() {
    // Python coroutine awaited from JS; JS Promise awaited from Python
    eq(await py.coro_add(Promise.resolve(40), 2), 42, "await Promise in Python, coroutine in JS");
    const delayed = new Promise((resolve) => Promise.resolve().then(() => resolve(10)));
    eq(await py.coro_add(delayed, 1), 11, "await a pending Promise");
    let failed;
    try { await py.coro_fail(); } catch (x) { failed = x; }
    eq(py.type_name(failed), "ValueError", "coroutine exception rejects the Task");
    const rej = new Error("rejected");
    eq(await py.await_reject(Promise.reject(rej)), rej, "rejection -> JsError in Python");
    const c = py.coro_add(Promise.resolve(1), 1);
    const p1 = c.then((v) => v), p2 = c.then((v) => v * 10);
    eq(await p1, 2, "then schedules one Task");
    eq(await p2, 20, "a second then observes the same Task");
}

async function testCycleCollected() {
    let collected = 0;
    const reg = new FinalizationRegistry(() => { collected++; });
    (function () {
        const o = {};
        const box = py.make_box();
        o.box = box;          // JS -> Python
        box.o = o;            // Python -> JS (setattr from JS)
        reg.register(o, "o");
        reg.register(box, "box");
    })();
    gc();
    await flushJobs();
    gc();
    await flushJobs();
    eq(collected, 2, "JS <-> Python reference cycle collected");
}

async function main() {
    testModules();
    testIdentity();
    testCalls();
    testNumbers();
    testJsObjectFromPython();
    testPythonObjectsFromJs();
    testExceptions();
    await testAsync();
    await testCycleCollected();
    print("PASS test_interop.js (" + checks + " checks)");
}

main().catch((e) => {
    print("FAIL test_interop.js: " + String(e));
    if (e && e.stack)
        print(e.stack);
    std.exit(1);
});
