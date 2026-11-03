function assert(actual, expected, message) {
    if (arguments.length == 1)
        expected = true;

    if (Object.is(actual, expected))
        return;

    if (actual !== null && expected !== null
    &&  typeof actual == 'object' && typeof expected == 'object'
    &&  actual.toString() === expected.toString())
        return;

    throw Error("assertion failed: got |" + actual + "|" +
                ", expected |" + expected + "|" +
                (message ? " (" + message + ")" : ""));
}

function assert_throws(expected_error, func)
{
    var err = false;
    try {
        func();
    } catch(e) {
        err = true;
        if (!(e instanceof expected_error)) {
            throw Error("unexpected exception type");
        }
    }
    if (!err) {
        throw Error("expected exception");
    }
}

// load more elaborate version of assert if available
try { __loadScript("test_assert.js"); } catch(e) {}

/*----------------*/

function test_op1()
{
    var r, a;
    r = 1 + 2;
    assert(r, 3, "1 + 2 === 3");

    r = 1 - 2;
    assert(r, -1, "1 - 2 === -1");

    r = -1;
    assert(r, -1, "-1 === -1");

    r = +2;
    assert(r, 2, "+2 === 2");

    r = 2 * 3;
    assert(r, 6, "2 * 3 === 6");

    r = 4 / 2;
    assert(r, 2, "4 / 2 === 2");

    r = 4 % 3;
    assert(r, 1, "4 % 3 === 3");

    r = 4 << 2;
    assert(r, 16, "4 << 2 === 16");

    r = 1 << 0;
    assert(r, 1, "1 << 0 === 1");

    r = 1 << 31;
    assert(r, -2147483648, "1 << 31 === -2147483648");

    r = 1 << 32;
    assert(r, 1, "1 << 32 === 1");

    r = (1 << 31) < 0;
    assert(r, true, "(1 << 31) < 0 === true");

    r = -4 >> 1;
    assert(r, -2, "-4 >> 1 === -2");

    r = -4 >>> 1;
    assert(r, 0x7ffffffe, "-4 >>> 1 === 0x7ffffffe");

    r = 1 & 1;
    assert(r, 1, "1 & 1 === 1");

    r = 0 | 1;
    assert(r, 1, "0 | 1 === 1");

    r = 1 ^ 1;
    assert(r, 0, "1 ^ 1 === 0");

    r = ~1;
    assert(r, -2, "~1 === -2");

    r = !1;
    assert(r, false, "!1 === false");

    assert((1 < 2), true, "(1 < 2) === true");

    assert((2 > 1), true, "(2 > 1) === true");

    assert(('b' > 'a'), true, "('b' > 'a') === true");

    assert(2 ** 8, 256, "2 ** 8 === 256");
}

function test_cvt()
{
    assert((NaN | 0) === 0);
    assert((Infinity | 0) === 0);
    assert(((-Infinity) | 0) === 0);
    assert(("12345" | 0) === 12345);
    assert(("0x12345" | 0) === 0x12345);
    assert(((4294967296 * 3 - 4) | 0) === -4);

    assert(("12345" >>> 0) === 12345);
    assert(("0x12345" >>> 0) === 0x12345);
    assert((NaN >>> 0) === 0);
    assert((Infinity >>> 0) === 0);
    assert(((-Infinity) >>> 0) === 0);
    assert(((4294967296 * 3 - 4) >>> 0) === (4294967296 - 4));
    assert((19686109595169230000).toString() === "19686109595169230000");
}

function test_eq()
{
    assert(null == undefined);
    assert(undefined == null);
    assert(true == 1);
    assert(0 == false);
    assert("" == 0);
    assert("123" == 123);
    assert("122" != 123);
    assert((new Number(1)) == 1);
    assert(2 == (new Number(2)));
    assert((new String("abc")) == "abc");
    assert({} != "abc");
}

function test_inc_dec()
{
    var a, r;

    a = 1;
    r = a++;
    assert(r === 1 && a === 2, true, "++");

    a = 1;
    r = ++a;
    assert(r === 2 && a === 2, true, "++");

    a = 1;
    r = a--;
    assert(r === 1 && a === 0, true, "--");

    a = 1;
    r = --a;
    assert(r === 0 && a === 0, true, "--");

    a = {x:true};
    a.x++;
    assert(a.x, 2, "++");

    a = {x:true};
    a.x--;
    assert(a.x, 0, "--");

    a = [true];
    a[0]++;
    assert(a[0], 2, "++");

    a = {x:true};
    r = a.x++;
    assert(r === 1 && a.x === 2, true, "++");

    a = {x:true};
    r = a.x--;
    assert(r === 1 && a.x === 0, true, "--");

    a = [true];
    r = a[0]++;
    assert(r === 1 && a[0] === 2, true, "++");

    a = [true];
    r = a[0]--;
    assert(r === 1 && a[0] === 0, true, "--");
}

function F(x)
{
    this.x = x;
}

function test_op2()
{
    var a, b;
    a = new Object;
    a.x = 1;
    assert(a.x, 1, "new");
    b = new F(2);
    assert(b.x, 2, "new");

    a = {x : 2};
    assert(("x" in a), true, "in");
    assert(("y" in a), false, "in");

    a = {};
    assert((a instanceof Object), true, "instanceof");
    assert((a instanceof String), false, "instanceof");

    assert((typeof 1), "number", "typeof");
    assert((typeof Object), "function", "typeof");
    assert((typeof null), "object", "typeof");
    assert((typeof unknown_var), "undefined", "typeof");

    a = {x: 1, if: 2, async: 3};
    assert(a.if === 2);
    assert(a.async === 3);
}

function test_delete()
{
    var a, err;

    a = {x: 1, y: 1};
    assert((delete a.x), true, "delete");
    assert(("x" in a), false, "delete");

    /* the following are not tested by test262 */
    assert(delete "abc"[100], true);

    err = false;
    try {
        delete null.a;
    } catch(e) {
        err = (e instanceof TypeError);
    }
    assert(err, true, "delete");

    err = false;
    try {
        a = { f() { delete super.a; } };
        a.f();
    } catch(e) {
        err = (e instanceof ReferenceError);
    }
    assert(err, true, "delete");
}

function test_constructor()
{
    function *G() {}
    let ex
    try { new G() } catch (ex_) { ex = ex_ }
    assert(ex instanceof TypeError)
    assert(ex.message, "G is not a constructor")
}

function test_prototype()
{
    var f = function f() { };
    assert(f.prototype.constructor, f, "prototype");

    var g = function g() { };
    /* QuickJS bug */
    Object.defineProperty(g, "prototype", { writable: false });
    assert(g.prototype.constructor, g, "prototype");
}

function test_arguments()
{
    function f2() {
        assert(arguments.length, 2, "arguments");
        assert(arguments[0], 1, "arguments");
        assert(arguments[1], 3, "arguments");
    }
    f2(1, 3);
}

function test_class()
{
    var o;
    class C {
        constructor() {
            this.x = 10;
        }
        f() {
            return 1;
        }
        static F() {
            return -1;
        }
        get y() {
            return 12;
        }
    };
    class D extends C {
        constructor() {
            super();
            this.z = 20;
        }
        g() {
            return 2;
        }
        static G() {
            return -2;
        }
        h() {
            return super.f();
        }
        static H() {
            return super["F"]();
        }
    }

    assert(C.F() === -1);
    assert(Object.getOwnPropertyDescriptor(C.prototype, "y").get.name === "get y");

    o = new C();
    assert(o.f() === 1);
    assert(o.x === 10);

    assert(D.F() === -1);
    assert(D.G() === -2);
    assert(D.H() === -1);

    o = new D();
    assert(o.f() === 1);
    assert(o.g() === 2);
    assert(o.x === 10);
    assert(o.z === 20);
    assert(o.h() === 1);

    /* test class name scope */
    var E1 = class E { static F() { return E; } };
    assert(E1 === E1.F());

    class S {
        static x = 42;
        static y = S.x;
        static z = this.x;
    }
    assert(S.x === 42);
    assert(S.y === 42);
    assert(S.z === 42);

    class P {
        get = () => "123";
        static() { return 42; }
    }
    assert(new P().get() === "123");
    assert(new P().static() === 42);
};

function test_template()
{
    var a, b;
    b = 123;
    a = `abc${b}d`;
    assert(a, "abc123d");

    a = String.raw `abc${b}d`;
    assert(a, "abc123d");

    a = "aaa";
    b = "bbb";
    assert(`aaa${a, b}ccc`, "aaabbbccc");
}

function test_template_skip()
{
    var a = "Bar";
    var { b = `${a + `a${a}` }baz` } = {};
    assert(b, "BaraBarbaz");
}

function test_object_literal()
{
    var x = 0, get = 1, set = 2, async = 3;
    a = { get: 2, set: 3, async: 4, get a(){ return this.get} };
    assert(JSON.stringify(a), '{"get":2,"set":3,"async":4,"a":2}');
    assert(a.a === 2);

    a = { x, get, set, async };
    assert(JSON.stringify(a), '{"x":0,"get":1,"set":2,"async":3}');
}

function test_regexp_skip()
{
    var a, b;
    [a, b = /abc\(/] = [1];
    assert(a === 1);

    [a, b =/abc\(/] = [2];
    assert(a === 2);
}

function test_labels()
{
    do x: { break x; } while(0);
    if (1)
        x: { break x; }
    else
        x: { break x; }
    with ({}) x: { break x; };
    while (0) x: { break x; };
}

function test_labels2()
{
    while (1) label: break
    var i = 0
    while (i < 3) label: {
        if (i > 0)
            break
        i++
    }
    assert(i, 1)
    for (;;) label: break
    for (i = 0; i < 3; i++) label: {
        if (i > 0)
            break
    }
    assert(i, 1)
}

function test_destructuring()
{
    function * g () { return 0; };
    var [x] = g();
    assert(x, void 0);
}

function test_spread()
{
    var x;
    x = [1, 2, ...[3, 4]];
    assert(x.toString(), "1,2,3,4");

    x = [ ...[ , ] ];
    assert(Object.getOwnPropertyNames(x).toString(), "0,length");
}

function test_function_length()
{
    assert( ((a, b = 1, c) => {}).length, 1);
    assert( (([a,b]) => {}).length, 1);
    assert( (({a,b}) => {}).length, 1);
    assert( ((c, [a,b] = 1, d) => {}).length, 1);
}

function test_argument_scope()
{
    var f;
    var c = "global";

    (function() {
        "use strict";
        // XXX: node only throws in strict mode
        f = function(a = eval("var arguments")) {};
        assert_throws(SyntaxError, f);
    })();

    f = function(a = eval("1"), b = arguments[0]) { return b; };
    assert(f(12), 12);

    f = function(a, b = arguments[0]) { return b; };
    assert(f(12), 12);

    f = function(a, b = () => arguments) { return b; };
    assert(f(12)()[0], 12);

    f = function(a = eval("1"), b = () => arguments) { return b; };
    assert(f(12)()[0], 12);

    (function() {
        "use strict";
        f = function(a = this) { return a; };
        assert(f.call(123), 123);

        f = function f(a = f) { return a; };
        assert(f(), f);

        f = function f(a = eval("f")) { return a; };
        assert(f(), f);
    })();

    f = (a = eval("var c = 1"), probe = () => c) => {
        var c = 2;
        assert(c, 2);
        assert(probe(), 1);
    }
    f();

    f = (a = eval("var arguments = 1"), probe = () => arguments) => {
        var arguments = 2;
        assert(arguments, 2);
        assert(probe(), 1);
    }
    f();

    f = function f(a = eval("var c = 1"), b = c, probe = () => c) {
        assert(b, 1);
        assert(c, 1);
        assert(probe(), 1)
    }
    f();

    assert(c, "global");
    f = function f(a, b = c, probe = () => c) {
        eval("var c = 1");
        assert(c, 1);
        assert(b, "global");
        assert(probe(), "global")
    }
    f();
    assert(c, "global");

    f = function f(a = eval("var c = 1"), probe = (d = eval("c")) => d) {
        assert(probe(), 1)
    }
    f();
}

function test_function_expr_name()
{
    var f;

    /* non strict mode test : assignment to the function name silently
       fails */

    f = function myfunc() {
        myfunc = 1;
        return myfunc;
    };
    assert(f(), f);

    f = function myfunc() {
        myfunc = 1;
        (() => {
            myfunc = 1;
        })();
        return myfunc;
    };
    assert(f(), f);

    f = function myfunc() {
        eval("myfunc = 1");
        return myfunc;
    };
    assert(f(), f);

    /* strict mode test : assignment to the function name raises a
       TypeError exception */

    f = function myfunc() {
        "use strict";
        myfunc = 1;
    };
    assert_throws(TypeError, f);

    f = function myfunc() {
        "use strict";
        (() => {
            myfunc = 1;
        })();
    };
    assert_throws(TypeError, f);

    f = function myfunc() {
        "use strict";
        eval("myfunc = 1");
    };
    assert_throws(TypeError, f);
}

function test_parse_semicolon()
{
    /* 'yield' or 'await' may not be considered as a token if the
       previous ';' is missing */
    function *f()
    {
        function func() {
        }
        yield 1;
        var h = x => x + 1
        yield 2;
    }
    async function g()
    {
        function func() {
        }
        await 1;
        var h = x => x + 1
        await 2;
    }
}

function test_parse_arrow_function()
{
    assert(typeof eval("() => {}\n() => {}"), "function");
    assert(eval("() => {}\n+1"), 1);
    assert(typeof eval("x => {}\n() => {}"), "function");
    assert(typeof eval("async () => {}\n() => {}"), "function");
    assert(typeof eval("async x => {}\n() => {}"), "function");
}

/* optional chaining tests not present in test262 */
function test_optional_chaining()
{
    var a, z;
    z = null;
    a = { b: { c: 2 } };
    assert(delete z?.b.c, true);
    assert(delete a?.b.c, true);
    assert(JSON.stringify(a), '{"b":{}}', "optional chaining delete");

    a = { b: { c: 2 } };
    assert(delete z?.b["c"], true);
    assert(delete a?.b["c"], true);
    assert(JSON.stringify(a), '{"b":{}}');

    a = {
        b() { return this._b; },
        _b: { c: 42 }
    };

    assert((a?.b)().c, 42);

    assert((a?.["b"])().c, 42);
}

function test_unicode_ident()
{
    var Ãµ = 3;
    assert(typeof õ, "undefined");
}

/* check global variable optimization */
function test_global_var_opt()
{
    var v2;
    (1, eval)('var gvar1'); /* create configurable global variables */

    gvar1 = 1;
    Object.defineProperty(globalThis, "gvar1", { writable: false });
    gvar1 = 2;
    assert(gvar1, 1);

    Object.defineProperty(globalThis, "gvar1", { get: function() { return "hello" },
                                                 set: function(v) { v2 = v; } });
    assert(gvar1, "hello");
    gvar1 = 3;
    assert(v2, 3);

    Object.defineProperty(globalThis, "gvar1", { value: 4, writable: true, configurable: true });
    assert(gvar1, 4);
    gvar1 = 6;
    
    delete gvar1;
    assert_throws(ReferenceError, function() { return gvar1 });
    gvar1 = 5;
    assert(gvar1, 5);
}

function test_number_literals()
{
    assert(0.1.a, undefined);
    assert(0x1.a, undefined);
    assert(0b1.a, undefined);
    assert(01.a, undefined);
    assert(0o1.a, undefined);
    assert_throws(SyntaxError, () => eval('0.a'));
}

/* variable lookups in functions with many variables (indexed by hash)
   must resolve like in small functions */
function test_many_bindings()
{
    var bodies = [
        'let x = 1, r = []; { let x = 2; r.push(x); { r.push(x); let y = x; { const x = 3; r.push(x, y); } } } r.push(x); return r.join();',
        'let r = []; try { r.push(t); } catch (e) { r.push(e.name); } let t = 1; r.push(t); return r.join();',
        'var r = []; try { throw 1; } catch (e) { var e = 2; r.push(e); } r.push(typeof e); return r.join();',
        'let fs = []; for (let i = 0; i < 3; i++) fs.push(() => i); return fs.map(f => f()).join();',
        'var r = []; { eval("var ev = 5"); r.push(ev); } let q = 1; r.push(eval("q + 1")); return r.join();',
        'var o = { w: 1 }, r = [], w = 2; with (o) { r.push(w); let w2 = 3; r.push(w2); } r.push(w); return r.join();',
        'return (function (a, b = a + 1, c = () => b) { let r = [a, b, c(), arguments.length]; { let a = 9; r.push(a); } return r.join(); })(1);',
        'var f = function g(n) { return n ? g(n - 1) + 1 : 0; }; return f(3) + typeof g;',
        'switch (1) { case 1: let s1 = 4; return s1; }',
        'class C { #p = 1; static #s = 2; static get(o) { return o.#p + C.#s; } } return C.get(new C());',
        'const k = 1; try { k = 2; } catch (e) { return e.name; } return "no";',
        'let a = 1; function f() { let b = 2; return function () { let c = 3; return () => a + b + c; }; } return f()()();',
        'let r = []; { let x = 1; { let x = 2; r.push(x); } let z = 0; r.push(x); { let x = 3; } let w = 0; r.push(x, z, w); } let x = 4; { let x = 5; } let z = 6; r.push(x, z); return r.join();',
        'var r = []; for (var v of [1, 2]) { let v2 = v * 2; r.push(() => v2); } return r.map(f => f()).join() + v;',
        'let r = []; label: { let x = 5; r.push(x); break label; } try { r.push(x); } catch (e) { r.push(e.name); } return r.join();',
        'let arguments2 = 1; return typeof arguments + arguments2;',
    ];
    var errors = [ 'let z; var z;', 'let z; { var z; }', 'const c; ', 'let d; let d;' ];
    var pad = "", i, j, f0, f1;
    for (i = 0; i < 40; i++)
        pad += "var pv" + i + " = " + i + "; let pl" + i + " = " + i + ";\n";
    for (i = 0; i < bodies.length; i++) {
        f0 = (0, eval)("(function () {\n" + bodies[i] + "\n})");
        f1 = (0, eval)("(function () {\n" + pad + bodies[i] + "\n})");
        assert(f1(), f0(), bodies[i]);
    }
    for (j = 0; j < errors.length; j++) {
        assert_throws(SyntaxError, () => (0, eval)("(function () {" + errors[j] + "})"));
        assert_throws(SyntaxError, () => (0, eval)("(function () {" + pad + errors[j] + "})"));
    }
}

test_op1();
test_cvt();
test_eq();
test_inc_dec();
test_op2();
test_constructor();
test_delete();
test_prototype();
test_arguments();
test_class();
test_template();
test_template_skip();
test_object_literal();
test_regexp_skip();
test_labels();
test_labels2();
test_destructuring();
test_spread();
test_function_length();
test_argument_scope();
test_function_expr_name();
test_parse_semicolon();
test_optional_chaining();
test_parse_arrow_function();
test_unicode_ident();
test_global_var_opt();
test_number_literals();
test_many_bindings();
