"use strict";

var status = 0;
var throw_errors = true;

function throw_error(msg) {
    if (throw_errors)
        throw Error(msg);
    console.log(msg);
    status = 1;
}

function assert(actual, expected, message) {
    function get_full_type(o) {
        var type = typeof(o);
        if (type === 'object') {
            if (o === null)
                return 'null';
            if (o.constructor && o.constructor.name)
                return o.constructor.name;
        }
        return type;
    }

    if (arguments.length == 1)
        expected = true;

    if (typeof actual === typeof expected) {
        if (actual === expected) {
            if (actual !== 0 || (1 / actual) === (1 / expected))
                return;
        }
        if (typeof actual === 'number') {
            if (isNaN(actual) && isNaN(expected))
                return true;
        }
        if (typeof actual === 'object') {
            if (actual !== null && expected !== null
            &&  actual.constructor === expected.constructor
            &&  actual.toString() === expected.toString())
                return;
        }
    }
    // Should output the source file and line number and extract
    //   the expression from the assert call
    throw_error("assertion failed: got " +
                get_full_type(actual) + ":|" + actual + "|, expected " +
                get_full_type(expected) + ":|" + expected + "|" +
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
            // Should output the source file and line number and extract
            //   the expression from the assert_throws() call
            throw_error("unexpected exception type");
            return;
        }
    }
    if (!err) {
        // Should output the source file and line number and extract
        //   the expression from the assert_throws() call
        throw_error("expected exception");
    }
}

// load more elaborate version of assert if available
try { __loadScript("test_assert.js"); } catch(e) {}

/*----------------*/

function my_func(a, b)
{
    return a + b;
}

function test_function()
{
    function f(a, b) {
        var i, tab = [];
        tab.push(this);
        for(i = 0; i < arguments.length; i++)
            tab.push(arguments[i]);
        return tab;
    }
    function constructor1(a) {
        this.x = a;
    }

    var r, g;

    r = my_func.call(null, 1, 2);
    assert(r, 3, "call");

    r = my_func.apply(null, [1, 2]);
    assert(r, 3, "apply");

    r = (function () { return 1; }).apply(null, undefined);
    assert(r, 1);

    assert_throws(TypeError, (function() {
        Reflect.apply((function () { return 1; }), null, undefined);
    }));

    r = new Function("a", "b", "return a + b;");
    assert(r(2,3), 5, "function");

    g = f.bind(1, 2);
    assert(g.length, 1);
    assert(g.name, "bound f");
    assert(g(3), [1,2,3]);

    g = constructor1.bind(null, 1);
    r = new g();
    assert(r.x, 1);
}

function test()
{
    var r, a, b, c, err;

    r = Error("hello");
    assert(r.message, "hello", "Error");

    a = new Object();
    a.x = 1;
    assert(a.x, 1, "Object");

    assert(Object.getPrototypeOf(a), Object.prototype, "getPrototypeOf");
    Object.defineProperty(a, "y", { value: 3, writable: true, configurable: true, enumerable: true });
    assert(a.y, 3, "defineProperty");

    Object.defineProperty(a, "z", { get: function () { return 4; }, set: function(val) { this.z_val = val; }, configurable: true, enumerable: true });
    assert(a.z, 4, "get");
    a.z = 5;
    assert(a.z_val, 5, "set");

    a = { get z() { return 4; }, set z(val) { this.z_val = val; } };
    assert(a.z, 4, "get");
    a.z = 5;
    assert(a.z_val, 5, "set");

    b = Object.create(a);
    assert(Object.getPrototypeOf(b), a, "create");
    c = {u:2};
    /* XXX: refcount bug in 'b' instead of 'a' */
    Object.setPrototypeOf(a, c);
    assert(Object.getPrototypeOf(a), c, "setPrototypeOf");

    a = {};
    assert(a.toString(), "[object Object]", "toString");

    a = {x:1};
    assert(Object.isExtensible(a), true, "extensible");
    Object.preventExtensions(a);

    err = false;
    try {
        a.y = 2;
    } catch(e) {
        err = true;
    }
    assert(Object.isExtensible(a), false, "extensible");
    assert(typeof a.y, "undefined", "extensible");
    assert(err, true, "extensible");
}

function test_enum()
{
    var a, tab;
    a = {x:1,
         "18014398509481984": 1,
         "9007199254740992": 1,
         "9007199254740991": 1,
         "4294967296": 1,
         "4294967295": 1,
         y:1,
         "4294967294": 1,
         "1": 2};
    tab = Object.keys(a);
//    console.log("tab=" + tab.toString());
    assert(tab, ["1","4294967294","x","18014398509481984","9007199254740992","9007199254740991","4294967296","4294967295","y"], "keys");
}

function test_array()
{
    var a, err;

    a = [1, 2, 3];
    assert(a.length, 3, "array");
    assert(a[2], 3, "array1");

    a = new Array(10);
    assert(a.length, 10, "array2");

    a = new Array(1, 2);
    assert(a.length === 2 && a[0] === 1 && a[1] === 2, true, "array3");

    a = [1, 2, 3];
    a.length = 2;
    assert(a.length === 2 && a[0] === 1 && a[1] === 2, true, "array4");

    a = [];
    a[1] = 10;
    a[4] = 3;
    assert(a.length, 5);

    a = [1,2];
    a.length = 5;
    a[4] = 1;
    a.length = 4;
    assert(a[4] !== 1, true, "array5");

    a = [1,2];
    a.push(3,4);
    assert(a.join(), "1,2,3,4", "join");

    a = [1,2,3,4,5];
    Object.defineProperty(a, "3", { configurable: false });
    err = false;
    try {
        a.length = 2;
    } catch(e) {
        err = true;
    }
    assert(err && a.toString() === "1,2,3,4");
}

function test_string()
{
    var a;
    a = String("abc");
    assert(a.length, 3, "string");
    assert(a[1], "b", "string");
    assert(a.charCodeAt(1), 0x62, "string");
    assert(String.fromCharCode(65), "A", "string");
    assert(String.fromCharCode.apply(null, [65, 66, 67]), "ABC", "string");
    assert(a.charAt(1), "b");
    assert(a.charAt(-1), "");
    assert(a.charAt(3), "");

    a = "abcd";
    assert(a.substring(1, 3), "bc", "substring");
    a = String.fromCharCode(0x20ac);
    assert(a.charCodeAt(0), 0x20ac, "unicode");
    assert(a, "€", "unicode");
    assert(a, "\u20ac", "unicode");
    assert(a, "\u{20ac}", "unicode");
    assert("a", "\x61", "unicode");

    a = "\u{10ffff}";
    assert(a.length, 2, "unicode");
    assert(a, "\u{dbff}\u{dfff}", "unicode");
    assert(a.codePointAt(0), 0x10ffff);
    assert(String.fromCodePoint(0x10ffff), a);

    assert("a".concat("b", "c"), "abc");

    assert("abcabc".indexOf("cab"), 2);
    assert("abcabc".indexOf("cab2"), -1);
    assert("abc".indexOf("c"), 2);

    assert("aaa".indexOf("a"), 0);
    assert("aaa".indexOf("a", NaN), 0);
    assert("aaa".indexOf("a", -Infinity), 0);
    assert("aaa".indexOf("a", -1), 0);
    assert("aaa".indexOf("a", -0), 0);
    assert("aaa".indexOf("a", 0), 0);
    assert("aaa".indexOf("a", 1), 1);
    assert("aaa".indexOf("a", 2), 2);
    assert("aaa".indexOf("a", 3), -1);
    assert("aaa".indexOf("a", 4), -1);
    assert("aaa".indexOf("a", Infinity), -1);

    assert("aaa".indexOf(""), 0);
    assert("aaa".indexOf("", NaN), 0);
    assert("aaa".indexOf("", -Infinity), 0);
    assert("aaa".indexOf("", -1), 0);
    assert("aaa".indexOf("", -0), 0);
    assert("aaa".indexOf("", 0), 0);
    assert("aaa".indexOf("", 1), 1);
    assert("aaa".indexOf("", 2), 2);
    assert("aaa".indexOf("", 3), 3);
    assert("aaa".indexOf("", 4), 3);
    assert("aaa".indexOf("", Infinity), 3);

    assert("aaa".lastIndexOf("a"), 2);
    assert("aaa".lastIndexOf("a", NaN), 2);
    assert("aaa".lastIndexOf("a", -Infinity), 0);
    assert("aaa".lastIndexOf("a", -1), 0);
    assert("aaa".lastIndexOf("a", -0), 0);
    assert("aaa".lastIndexOf("a", 0), 0);
    assert("aaa".lastIndexOf("a", 1), 1);
    assert("aaa".lastIndexOf("a", 2), 2);
    assert("aaa".lastIndexOf("a", 3), 2);
    assert("aaa".lastIndexOf("a", 4), 2);
    assert("aaa".lastIndexOf("a", Infinity), 2);

    assert("aaa".lastIndexOf(""), 3);
    assert("aaa".lastIndexOf("", NaN), 3);
    assert("aaa".lastIndexOf("", -Infinity), 0);
    assert("aaa".lastIndexOf("", -1), 0);
    assert("aaa".lastIndexOf("", -0), 0);
    assert("aaa".lastIndexOf("", 0), 0);
    assert("aaa".lastIndexOf("", 1), 1);
    assert("aaa".lastIndexOf("", 2), 2);
    assert("aaa".lastIndexOf("", 3), 3);
    assert("aaa".lastIndexOf("", 4), 3);
    assert("aaa".lastIndexOf("", Infinity), 3);

    assert("a,b,c".split(","), ["a","b","c"]);
    assert(",b,c".split(","), ["","b","c"]);
    assert("a,b,".split(","), ["a","b",""]);

    assert("aaaa".split(), [ "aaaa" ]);
    assert("aaaa".split(undefined, 0), [ ]);
    assert("aaaa".split(""), [ "a", "a", "a", "a" ]);
    assert("aaaa".split("", 0), [ ]);
    assert("aaaa".split("", 1), [ "a" ]);
    assert("aaaa".split("", 2), [ "a", "a" ]);
    assert("aaaa".split("a"), [ "", "", "", "", "" ]);
    assert("aaaa".split("a", 2), [ "", "" ]);
    assert("aaaa".split("aa"), [ "", "", "" ]);
    assert("aaaa".split("aa", 0), [ ]);
    assert("aaaa".split("aa", 1), [ "" ]);
    assert("aaaa".split("aa", 2), [ "", "" ]);
    assert("aaaa".split("aaa"), [ "", "a" ]);
    assert("aaaa".split("aaaa"), [ "", "" ]);
    assert("aaaa".split("aaaaa"), [ "aaaa" ]);
    assert("aaaa".split("aaaaa", 0), [  ]);
    assert("aaaa".split("aaaaa", 1), [ "aaaa" ]);

    assert(eval('"\0"'), "\0");

    assert("abc".padStart(Infinity, ""), "abc");

    /* filled with one character, widening 8 bit strings */
    assert("x".repeat(0), "");
    assert("x".repeat(3), "xxx");
    assert("\u2713".repeat(3), "\u2713\u2713\u2713");
    assert("ab".padStart(5, "\u2713"), "\u2713\u2713\u2713ab");
    assert("ab".padEnd(5, "\xe9"), "ab\xe9\xe9\xe9");
    assert("\u0100b".padEnd(4, "c"), "\u0100bcc");
    assert("\u0100b".padStart(4, "\u2713"), "\u2713\u2713\u0100b");
}

function test_math()
{
    var a;
    a = 1.4;
    assert(Math.floor(a), 1);
    assert(Math.ceil(a), 2);
    assert(Math.imul(0x12345678, 123), -1088058456);
    assert(Math.imul(0xB505, 0xB504), 2147441940);
    assert(Math.imul(0xB505, 0xB505), -2147479015);
    assert(Math.imul((-2)**31, (-2)**31), 0);
    assert(Math.imul(2**31-1, 2**31-1), 1);
    assert(Math.fround(0.1), 0.10000000149011612);
    assert(Math.hypot(), 0);
    assert(Math.hypot(-2), 2);
    assert(Math.hypot(3, 4), 5);
    assert(Math.abs(Math.hypot(3, 4, 5) - 7.0710678118654755) <= 1e-15);
    assert(Math.sumPrecise([1,Number.EPSILON/2,Number.MIN_VALUE]), 1.0000000000000002);
}

function test_number()
{
    assert(parseInt("123"), 123);
    assert(parseInt("  123r"), 123);
    assert(parseInt("0x123"), 0x123);
    assert(parseInt("0o123"), 0);
    assert(+"  123   ", 123);
    assert(+"0b111", 7);
    assert(+"0o123", 83);
    assert(parseFloat("2147483647"), 2147483647);
    assert(parseFloat("2147483648"), 2147483648);
    assert(parseFloat("-2147483647"), -2147483647);
    assert(parseFloat("-2147483648"), -2147483648);
    assert(parseFloat("0x1234"), 0);
    assert(parseFloat("Infinity"), Infinity);
    assert(parseFloat("-Infinity"), -Infinity);
    assert(parseFloat("123.2"), 123.2);
    assert(parseFloat("123.2e3"), 123200);
    assert(Number.isNaN(Number("+")));
    assert(Number.isNaN(Number("-")));
    assert(Number.isNaN(Number("\x00a")));

    assert((1-2**-53).toString(12), "0.bbbbbbbbbbbbbba");
    assert((1000000000000000128).toString(), "1000000000000000100");
    assert((1000000000000000128).toFixed(0), "1000000000000000128");
    assert((25).toExponential(0), "3e+1");
    assert((-25).toExponential(0), "-3e+1");
    assert((2.5).toPrecision(1), "3");
    assert((-2.5).toPrecision(1), "-3");
    assert((25).toPrecision(1) === "3e+1");
    assert((1.125).toFixed(2), "1.13");
    assert((-1.125).toFixed(2), "-1.13");
    assert((0.5).toFixed(0), "1");
    assert((-0.5).toFixed(0), "-1");
    assert((-1e-10).toFixed(0), "-0");

    assert((1.3).toString(7), "1.2046204620462046205");
    assert((1.3).toString(35), "1.ahhhhhhhhhm");
}

function test_eval2()
{
    var g_call_count = 0;
    /* force non strict mode for f1 and f2 */
    var f1 = new Function("eval", "eval(1, 2)");
    var f2 = new Function("eval", "eval(...[1, 2])");
    function g(a, b) {
        assert(a, 1);
        assert(b, 2);
        g_call_count++;
    }
    f1(g);
    f2(g);
    assert(g_call_count, 2);
}

function test_eval()
{
    function f(b) {
        var x = 1;
        return eval(b);
    }
    var r, a;

    r = eval("1+1;");
    assert(r, 2, "eval");

    r = eval("var my_var=2; my_var;");
    assert(r, 2, "eval");
    assert(typeof my_var, "undefined");

    assert(eval("if (1) 2; else 3;"), 2);
    assert(eval("if (0) 2; else 3;"), 3);

    assert(f.call(1, "this"), 1);

    a = 2;
    assert(eval("a"), 2);

    eval("a = 3");
    assert(a, 3);

    assert(f("arguments.length", 1), 2);
    assert(f("arguments[1]", 1), 1);

    a = 4;
    assert(f("a"), 4);
    f("a=3");
    assert(a, 3);

    test_eval2();
}

function test_typed_array()
{
    var buffer, a, i, str;

    a = new Uint8Array(4);
    assert(a.length, 4);
    for(i = 0; i < a.length; i++)
        a[i] = i;
    assert(a.join(","), "0,1,2,3");
    a[0] = -1;
    assert(a[0], 255);

    a = new Int8Array(3);
    a[0] = 255;
    assert(a[0], -1);

    a = new Int32Array(3);
    a[0] = Math.pow(2, 32) - 1;
    assert(a[0], -1);
    assert(a.BYTES_PER_ELEMENT, 4);

    a = new Uint8ClampedArray(4);
    a[0] = -100;
    a[1] = 1.5;
    a[2] = 0.5;
    a[3] = 1233.5;
    assert(a.toString(), "0,2,0,255");

    buffer = new ArrayBuffer(16);
    assert(buffer.byteLength, 16);
    a = new Uint32Array(buffer, 12, 1);
    assert(a.length, 1);
    a[0] = -1;

    a = new Uint16Array(buffer, 2);
    a[0] = -1;

    a = new Float16Array(buffer, 8, 1);
    a[0] = 1;

    a = new Float32Array(buffer, 8, 1);
    a[0] = 1;

    a = new Uint8Array(buffer);

    str = a.toString();
    /* test little and big endian cases */
    if (str !== "0,0,255,255,0,0,0,0,0,0,128,63,255,255,255,255" &&
        str !== "0,0,255,255,0,0,0,0,63,128,0,0,255,255,255,255") {
        assert(false);
    }

    assert(a.buffer, buffer);

    a = new Uint8Array([1, 2, 3, 4]);
    assert(a.toString(), "1,2,3,4");
    a.set([10, 11], 2);
    assert(a.toString(), "1,2,10,11");

    // https://github.com/quickjs-ng/quickjs/issues/1208
    buffer = new ArrayBuffer(16);
    a = new Uint8Array(buffer);
    a.fill(42);
    assert(a[0], 42);
    buffer.transfer();
    assert(a[0], undefined);

    var desc;
    buffer = new ArrayBuffer(2);
    a = new Uint16Array(buffer);
    desc = Object.getOwnPropertyDescriptor(a, "0");
    a[0] = 42;
    assert(a[0], 42);
    Object.defineProperty(a, "0", {value: 1337});
    assert(a[0], 1337);
    assert(desc.writable, true);
    assert(desc.enumerable, true);
    assert(desc.configurable, true);

    buffer = new ArrayBuffer(2).sliceToImmutable();
    a = new Uint16Array(buffer);
    desc = Object.getOwnPropertyDescriptor(a, "0");
    assert_throws(TypeError, () => { a[0] = 42; });
    assert(a[0], 0);
    assert_throws(TypeError, () => Object.defineProperty(a, "0", {value: 1337}));
    assert(a[0], 0);
    assert(desc.writable, false);
    assert(desc.enumerable, true);
    assert(desc.configurable, false);
}

function test_immutable_array_buffer()
{
    var src, buf, a, calls, r;
    var tracked = { valueOf() { calls.push("valueOf"); return 1; } };

    /* transferToImmutable() detaches the source */
    src = new ArrayBuffer(4, { maxByteLength: 8 });
    new Uint8Array(src).set([1, 2, 3, 4]);
    buf = src.transferToImmutable(6);
    assert(src.detached, true);
    assert(buf.immutable, true);
    assert(buf.resizable, false);
    assert(new Uint8Array(buf).join(), "1,2,3,4,0,0");
    assert(new Uint8Array(new Uint8Array([5, 6]).buffer.transferToImmutable(1)).join(), "5");
    assert(new ArrayBuffer(1).immutable, false);
    assert_throws(TypeError, () => Object.getOwnPropertyDescriptor(ArrayBuffer.prototype, "immutable").get.call(new SharedArrayBuffer(1)));

    /* an immutable ArrayBuffer is read-only and cannot be detached nor resized */
    for (var f of [b => b.resize(0), b => b.transfer(), b => b.transferToFixedLength(),
                   b => b.transferToImmutable()]) {
        assert_throws(TypeError, () => f(buf));
        assert(buf.detached, false);
    }
    calls = [];
    assert_throws(TypeError, () => buf.resize(tracked));
    assert(calls.length, 0);

    /* slicing reads the buffer: the source can be immutable */
    assert(buf.slice(1, 3).immutable, false);
    assert(new Uint8Array(buf.slice(1, 3)).join(), "2,3");
    r = buf.sliceToImmutable(-3);
    assert(r.immutable, true);
    assert(new Uint8Array(r).join(), "4,0,0");
    /* sliceToImmutable() ignores the species constructor */
    src = new ArrayBuffer(4);
    src.constructor = { [Symbol.species]: function() { throw Error("species"); } };
    assert(src.sliceToImmutable(1).byteLength, 3);
    src.constructor = { [Symbol.species]: function() { return buf.sliceToImmutable(); } };
    assert_throws(TypeError, () => src.slice());
    /* shrinking the source below the end is a RangeError */
    src = new ArrayBuffer(8, { maxByteLength: 8 });
    assert_throws(RangeError, () => src.sliceToImmutable(0, { valueOf() { src.resize(2); return 4; } }));
    /* detaching it is a TypeError */
    assert_throws(TypeError, () => src.sliceToImmutable({ valueOf() { src.transfer(); return 0; } }));

    a = new Uint8Array(buf);
    /* the elements are non-writable, non-configurable data properties */
    assert(Reflect.set(a, "0", 9), false);
    assert(Reflect.set(a, "10", 9), false);
    calls = [];
    assert(Reflect.set(a, "1", tracked), false);
    assert(calls.length, 0);
    assert(Reflect.set(Object.create(a), "0", 9), false);
    assert(Function("a", "a[0] = 9; a[-1] = 9; return a[0]")(a), 1);
    assert_throws(TypeError, () => { a[10] = 1; });
    assert(Reflect.defineProperty(a, "0", { value: 1 }), true);
    assert(Reflect.defineProperty(a, "0", { value: "1" }), false);
    assert(Reflect.defineProperty(a, "0", { value: 2 }), false);
    assert(Reflect.defineProperty(a, "0", { writable: true }), false);
    assert(Reflect.defineProperty(a, "0", { configurable: true }), false);
    assert(Reflect.defineProperty(a, "0", { enumerable: false }), false);
    assert(Reflect.defineProperty(a, "0", { get() { return 1; } }), false);
    assert(Reflect.defineProperty(a, "0", { writable: false, configurable: false, enumerable: true }), true);
    assert(Object.isFrozen(Object.freeze(new Uint8Array(buf))), true);

    /* mutating methods throw before reading their arguments */
    for (f of [() => a.fill(tracked), () => a.set([tracked], tracked),
               () => a.copyWithin(tracked, 0), () => a.reverse(), () => a.sort(),
               () => a.setFromHex("ff"), () => a.setFromBase64("AA=="),
               () => new DataView(buf).setUint8(tracked, tracked),
               () => new DataView(buf).setBigInt64(tracked, 1n),
               () => Atomics.store(new Int8Array(buf), tracked, tracked),
               () => Atomics.add(new Int8Array(buf), tracked, tracked),
               () => Atomics.compareExchange(new Int8Array(buf), tracked, 1, 2)]) {
        calls = [];
        assert_throws(TypeError, f);
        assert(calls.length, 0);
    }
    assert(a.join(), "1,2,3,4,0,0");

    /* reading and copying are allowed */
    assert(a.toReversed().join(), "0,0,4,3,2,1");
    assert(a.toSorted().join(), "0,0,1,2,3,4");
    assert(a.with(0, 7).join(), "7,2,3,4,0,0");
    assert(a.slice(2, 4).join(), "3,4");
    assert(a.subarray(2).buffer, buf);
    assert(a.map(x => x * 2).join(), "2,4,6,8,0,0");
    assert(Atomics.load(new Int8Array(buf), 2), 3);
    assert(new DataView(buf).getUint16(0, true), 0x201);

    /* results written by TypedArray methods must be mutable */
    class Immutable extends Uint8Array {
        constructor(n) { super(typeof n === "number" ? new ArrayBuffer(n).transferToImmutable() : n); }
    }
    var im = new Immutable(4);
    assert_throws(TypeError, () => Immutable.from([1, 2]));
    assert_throws(TypeError, () => Immutable.of(1, 2));
    assert_throws(TypeError, () => im.map(x => x));
    assert_throws(TypeError, () => im.filter(x => true));
    assert_throws(TypeError, () => im.slice());
    assert(im.subarray(1) instanceof Immutable, true);
}

/* return [s, line_num, col_num] where line_num and col_num are the
   position of the '@' character in 'str'. 's' is str without the '@'
   character */
function get_string_pos(str)
{
    var p, line_num, col_num, s, q, r;
    p = str.indexOf('@');
    assert(p >= 0, true);
    q = 0;
    line_num = 1;
    for(;;) {
        r = str.indexOf('\n', q);
        if (r < 0 || r >= p)
            break;
        q = r + 1;
        line_num++;
    }
    col_num = p - q + 1;
    s = str.slice(0, p) + str.slice(p + 1);
    return [s, line_num, col_num];
}

function check_error_pos(e, expected_error, line_num, col_num, level)
{
    var expected_pos, tab, line;
    level |= 0;
    expected_pos = ":" + line_num + ":" + col_num;
    tab = e.stack.split("\n");
    line = tab[level];
    if (line.slice(-1) == ')')
        line = line.slice(0, -1);
    if (line.indexOf(expected_pos) < 0) {
        throw_error("unexpected line or column number. error=" + e.message +
                    ".got |" + line + "|, expected |" + expected_pos + "|");
    }
}

function assert_json_error(str, line_num, col_num)
{
    var err = false;
    var expected_pos, tab;

    tab = get_string_pos(str);
    
    try {
        JSON.parse(tab[0]);
    } catch(e) {
        err = true;
        if (!(e instanceof SyntaxError)) {
            throw_error("unexpected exception type");
            return;
        }
        /* XXX: the way quickjs returns JSON errors is not similar to Node or spiderMonkey */
        check_error_pos(e, SyntaxError, tab[1], tab[2]);
    }
    if (!err) {
        throw_error("expected exception");
    }
}

function test_json()
{
    var a, s;
    s = '{"x":1,"y":true,"z":null,"a":[1,2,3],"s":"str"}';
    a = JSON.parse(s);
    assert(a.x, 1);
    assert(a.y, true);
    assert(a.z, null);
    assert(JSON.stringify(a), s);

    /* indentation test */
    assert(JSON.stringify([[{x:1,y:{},z:[]},2,3]],undefined,1),
`[
 [
  {
   "x": 1,
   "y": {},
   "z": []
  },
  2,
  3
 ]
]`);

    assert_json_error('\n"  \\@x"');
    assert_json_error('\n{ "a": @x }"');
}

/* duplicate keys found with a hash of their atoms */
function test_duplicate_keys()
{
    var o, ks, i, t, ne, sym, p, n;
    /* JSON.stringify() replacer array: first occurrence order, numbers
       and String/Number objects converted to strings, others ignored */
    o = { a: 1, b: 2, 1: 3, 2: 4 };
    assert(JSON.stringify(o, ["b", "a", "b", 1, "1", new String("a"),
                              new Number(2), {}, null, true]),
           '{"b":2,"a":1,"1":3,"2":4}');
    assert(JSON.stringify(o, []), "{}");
    ks = [];
    o = {};
    for (i = 0; i < 1000; i++) {
        ks.push("k" + (i % 500));
        o["k" + i] = i;
    }
    assert(JSON.stringify(o, ks), JSON.stringify(o, ks.slice(0, 500)));

    /* proxy ownKeys trap result */
    assert_throws(TypeError, () => Reflect.ownKeys(new Proxy({}, { ownKeys: () => ["a", "b", "a"] })));
    sym = Symbol();
    assert_throws(TypeError, () => Reflect.ownKeys(new Proxy({}, { ownKeys: () => [sym, "a", sym] })));
    t = {};
    Object.defineProperty(t, "x", { value: 1, configurable: false });
    assert_throws(TypeError, () => Reflect.ownKeys(new Proxy(t, { ownKeys: () => ["y"] })));
    assert(Reflect.ownKeys(new Proxy(t, { ownKeys: () => ["y", "x"] })).join(), "y,x");
    ne = Object.preventExtensions({ a: 1, b: 2 });
    assert_throws(TypeError, () => Reflect.ownKeys(new Proxy(ne, { ownKeys: () => ["a"] })));
    assert_throws(TypeError, () => Reflect.ownKeys(new Proxy(ne, { ownKeys: () => ["a", "b", "c"] })));
    assert(Reflect.ownKeys(new Proxy(ne, { ownKeys: () => ["b", "a"] })).join(), "b,a");
    p = new Proxy({}, { ownKeys: () => ks.slice(0, 500) });
    assert(Reflect.ownKeys(p).length, 500);
    assert_throws(TypeError, () => Reflect.ownKeys(new Proxy({}, { ownKeys: () => ks })));

    /* short lists are searched linearly, longer ones with a hash table:
       same results on both sides of the limits (16 and 96 keys) */
    for (n = 1; n <= 130; n++) {
        ks = [];
        o = {};
        for (i = 0; i < n; i++) {
            ks.push(i % 3 == 0 ? String(i) : "k" + i);
            o[ks[i]] = i;
        }
        t = ks.slice().reverse();
        assert(Reflect.ownKeys(new Proxy({}, { ownKeys: () => t })).join(), t.join());
        assert_throws(TypeError, () => Reflect.ownKeys(new Proxy({}, { ownKeys: () => t.concat(t[0]) })));
        assert_throws(TypeError, () => Reflect.ownKeys(new Proxy({}, { ownKeys: () => t.concat(t[n - 1]) })));
        ne = Object.freeze(Object.assign({}, o));
        assert(Reflect.ownKeys(new Proxy(ne, { ownKeys: () => t })).join(), t.join());
        assert_throws(TypeError, () => Reflect.ownKeys(new Proxy(ne, { ownKeys: () => t.slice(1) })));
        assert_throws(TypeError, () => Reflect.ownKeys(new Proxy(ne, { ownKeys: () => t.concat("z") })));
        assert_throws(TypeError, () => Reflect.ownKeys(new Proxy(ne, { ownKeys: () => t.slice(1).concat(t[1]) })));
        assert(JSON.stringify(o, t.concat(t[n - 1], "z", t[0])),
               "{" + t.map(k => '"' + k + '":' + o[k]).join() + "}");
        assert(JSON.stringify(o, [t[0]].concat(t)),
               "{" + t.map(k => '"' + k + '":' + o[k]).join() + "}");
    }

    /* large lists, O(n^2) before: they take seconds if this regresses */
    n = 100000;
    ks = [];
    o = {};
    for (i = 0; i < n; i++) {
        ks.push("k" + i);
        o["k" + i] = i;
    }
    ks.push("k0");
    assert(JSON.stringify(o, ks), JSON.stringify(o));
    assert_throws(TypeError, () => Reflect.ownKeys(new Proxy(o, { ownKeys: () => ks })));
    ks.pop();
    assert(Reflect.ownKeys(new Proxy(o, { ownKeys: () => ks })).length, n);
}

function test_date()
{
    // Date Time String format is YYYY-MM-DDTHH:mm:ss.sssZ
    // accepted date formats are: YYYY, YYYY-MM and YYYY-MM-DD
    // accepted time formats are: THH:mm, THH:mm:ss, THH:mm:ss.sss
    // expanded years are represented with 6 digits prefixed by + or -
    // -000000 is invalid.
    // A string containing out-of-bounds or nonconforming elements
    //   is not a valid instance of this format.
    // Hence the fractional part after . should have 3 digits and how
    // a different number of digits is handled is implementation defined.
    assert(Date.parse(""), NaN);
    assert(Date.parse("2000"), 946684800000);
    assert(Date.parse("2000-01"), 946684800000);
    assert(Date.parse("2000-01-01"), 946684800000);
    //assert(Date.parse("2000-01-01T"), NaN);
    //assert(Date.parse("2000-01-01T00Z"), NaN);
    assert(Date.parse("2000-01-01T00:00Z"), 946684800000);
    assert(Date.parse("2000-01-01T00:00:00Z"), 946684800000);
    assert(Date.parse("2000-01-01T00:00:00.1Z"), 946684800100);
    assert(Date.parse("2000-01-01T00:00:00.10Z"), 946684800100);
    assert(Date.parse("2000-01-01T00:00:00.100Z"), 946684800100);
    assert(Date.parse("2000-01-01T00:00:00.1000Z"), 946684800100);
    assert(Date.parse("2000-01-01T00:00:00+00:00"), 946684800000);
    //assert(Date.parse("2000-01-01T00:00:00+00:30"), 946686600000);
    var d = new Date("2000T00:00");  // Jan 1st 2000, 0:00:00 local time
    assert(typeof d === 'object' && d.toString() != 'Invalid Date');
    assert((new Date('Jan 1 2000')).toISOString(),
           d.toISOString());
    assert((new Date('Jan 1 2000 00:00')).toISOString(),
           d.toISOString());
    assert((new Date('Jan 1 2000 00:00:00')).toISOString(),
           d.toISOString());
    assert((new Date('Jan 1 2000 00:00:00 GMT+0100')).toISOString(),
           '1999-12-31T23:00:00.000Z');
    assert((new Date('Jan 1 2000 00:00:00 GMT+0200')).toISOString(),
           '1999-12-31T22:00:00.000Z');
    assert((new Date('Sat Jan 1 2000')).toISOString(),
           d.toISOString());
    assert((new Date('Sat Jan 1 2000 00:00')).toISOString(),
           d.toISOString());
    assert((new Date('Sat Jan 1 2000 00:00:00')).toISOString(),
           d.toISOString());
    assert((new Date('Sat Jan 1 2000 00:00:00 GMT+0100')).toISOString(),
           '1999-12-31T23:00:00.000Z');
    assert((new Date('Sat Jan 1 2000 00:00:00 GMT+0200')).toISOString(),
           '1999-12-31T22:00:00.000Z');

    var d = new Date(1506098258091);
    assert(d.toISOString(), "2017-09-22T16:37:38.091Z");
    d.setUTCHours(18, 10, 11);
    assert(d.toISOString(), "2017-09-22T18:10:11.091Z");
    var a = Date.parse(d.toISOString());
    assert((new Date(a)).toISOString(), d.toISOString());

    assert((new Date("2020-01-01T01:01:01.123Z")).toISOString(),
                     "2020-01-01T01:01:01.123Z");
    /* implementation defined behavior */
    assert((new Date("2020-01-01T01:01:01.1Z")).toISOString(),
                     "2020-01-01T01:01:01.100Z");
    assert((new Date("2020-01-01T01:01:01.12Z")).toISOString(),
                     "2020-01-01T01:01:01.120Z");
    assert((new Date("2020-01-01T01:01:01.1234Z")).toISOString(),
                     "2020-01-01T01:01:01.123Z");
    assert((new Date("2020-01-01T01:01:01.12345Z")).toISOString(),
                     "2020-01-01T01:01:01.123Z");
    assert((new Date("2020-01-01T01:01:01.1235Z")).toISOString(),
                     "2020-01-01T01:01:01.123Z");
    assert((new Date("2020-01-01T01:01:01.9999Z")).toISOString(),
                     "2020-01-01T01:01:01.999Z");

    assert(Date.UTC(2017), 1483228800000);
    assert(Date.UTC(2017, 9), 1506816000000);
    assert(Date.UTC(2017, 9, 22), 1508630400000);
    assert(Date.UTC(2017, 9, 22, 18), 1508695200000);
    assert(Date.UTC(2017, 9, 22, 18, 10), 1508695800000);
    assert(Date.UTC(2017, 9, 22, 18, 10, 11), 1508695811000);
    assert(Date.UTC(2017, 9, 22, 18, 10, 11, 91), 1508695811091);

    assert(Date.UTC(NaN), NaN);
    assert(Date.UTC(2017, NaN), NaN);
    assert(Date.UTC(2017, 9, NaN), NaN);
    assert(Date.UTC(2017, 9, 22, NaN), NaN);
    assert(Date.UTC(2017, 9, 22, 18, NaN), NaN);
    assert(Date.UTC(2017, 9, 22, 18, 10, NaN), NaN);
    assert(Date.UTC(2017, 9, 22, 18, 10, 11, NaN), NaN);
    assert(Date.UTC(2017, 9, 22, 18, 10, 11, 91, NaN), 1508695811091);

    // TODO: Fix rounding errors on Windows/Cygwin.
    if (!(typeof os !== 'undefined' && ['win32', 'cygwin'].includes(os.platform))) {
        // from test262/test/built-ins/Date/UTC/fp-evaluation-order.js
        assert(Date.UTC(1970, 0, 1, 80063993375, 29, 1, -288230376151711740), 29312,
               'order of operations / precision in MakeTime');
        assert(Date.UTC(1970, 0, 213503982336, 0, 0, 0, -18446744073709552000), 34447360,
               'precision in MakeDate');
    }
    //assert(Date.UTC(2017 - 1e9, 9 + 12e9), 1506816000000);  // node fails this
    assert(Date.UTC(2017, 9, 22 - 1e10, 18 + 24e10), 1508695200000);
    assert(Date.UTC(2017, 9, 22, 18 - 1e10, 10 + 60e10), 1508695800000);
    assert(Date.UTC(2017, 9, 22, 18, 10 - 1e10, 11 + 60e10), 1508695811000);
    assert(Date.UTC(2017, 9, 22, 18, 10, 11 - 1e12, 91 + 1000e12), 1508695811091);
}

function test_regexp()
{
    var a, str;
    str = "abbbbbc";
    a = /(b+)c/.exec(str);
    assert(a[0], "bbbbbc");
    assert(a[1], "bbbbb");
    assert(a.index, 1);
    assert(a.input, str);
    a = /(b+)c/.test(str);
    assert(a, true);
    assert(/\x61/.exec("a")[0], "a");
    assert(/\u0061/.exec("a")[0], "a");
    assert(/\ca/.exec("\x01")[0], "\x01");
    assert(/\\a/.exec("\\a")[0], "\\a");
    assert(/\c0/.exec("\\c0")[0], "\\c0");

    a = /(\.(?=com|org)|\/)/.exec("ah.com");
    assert(a.index === 2 && a[0] === ".");

    a = /(\.(?!com|org)|\/)/.exec("ah.com");
    assert(a, null);

    a = /(?=(a+))/.exec("baaabac");
    assert(a.index === 1 && a[0] === "" && a[1] === "aaa");

    a = /(z)((a+)?(b+)?(c))*/.exec("zaacbbbcac");
    assert(a, ["zaacbbbcac","z","ac","a",,"c"]);

    a = eval("/\0a/");
    assert(a.toString(), "/\0a/");
    assert(a.exec("\0a")[0], "\0a");

    assert(/{1a}/.toString(), "/{1a}/");
    a = /a{1+/.exec("a{11");
    assert(a, ["a{11"]);

    /* test zero length matches */
    a = /(?:(?=(abc)))a/.exec("abc");
    assert(a, ["a", "abc"]);
    a = /(?:(?=(abc)))?a/.exec("abc");
    assert(a, ["a", undefined]);
    a = /(?:(?=(abc))){0,2}a/.exec("abc");
    assert(a, ["a", undefined]);
    a = /(?:|[\w])+([0-9])/.exec("123a23");
    assert(a, ["123a23", "3"]);
    a = /()*?a/.exec(",");
    assert(a, null);

    /* test \b escape */
    assert(/[\q{a\b}]/.test("a\b"), true);
    assert(/[\b]/.test("\b"), true);
    
    /* test case insensitive matching (test262 hardly tests it) */
    assert("aAbBcC#4".replace(/\p{Lower}/gu,"X"), "XAXBXC#4");

    assert("aAbBcC#4".replace(/\p{Lower}/gui,"X"), "XXXXXX#4");
    assert("aAbBcC#4".replace(/\p{Upper}/gui,"X"), "XXXXXX#4");
    assert("aAbBcC#4".replace(/\P{Lower}/gui,"X"), "XXXXXXXX");
    assert("aAbBcC#4".replace(/\P{Upper}/gui,"X"), "XXXXXXXX");
    assert("aAbBcC".replace(/[^b]/gui, "X"), "XXbBXX");
    assert("aAbBcC".replace(/[^A-B]/gui, "X"), "aAbBXX");

    assert("aAbBcC#4".replace(/\p{Lower}/gvi,"X"), "XXXXXX#4");
    assert("aAbBcC#4".replace(/\P{Lower}/gvi,"X"), "aAbBcCXX");
    assert("aAbBcC#4".replace(/[^\P{Lower}]/gvi,"X"), "XXXXXX#4");
    assert("aAbBcC#4".replace(/\P{Upper}/gvi,"X"), "aAbBcCXX");
    assert("aAbBcC".replace(/[^b]/gvi, "X"), "XXbBXX");
    assert("aAbBcC".replace(/[^A-B]/gvi, "X"), "aAbBXX");
    assert("aAbBcC".replace(/[[a-c]&&B]/gvi, "X"), "aAXXcC");
    assert("aAbBcC".replace(/[[a-c]--B]/gvi, "X"), "XXbBXX");
    
    assert("abcAbC".replace(/[\q{AbC}]/gvi,"X"), "XX");
    /* Note: SpiderMonkey and v8 may not be correct */
    assert("abcAbC".replace(/[\q{BC|A}]/gvi,"X"), "XXXX");
    assert("abcAbC".replace(/[\q{BC|A}--a]/gvi,"X"), "aXAX");

    /* case where lastIndex points to the second element of a
       surrogate pair */
    a = /(?:)/gu;
    a.lastIndex = 1;
    a.exec("🐱");
    assert(a.lastIndex, 0);

    a.lastIndex = 1;
    a.exec("a\udc00");
    assert(a.lastIndex, 1);

    a = /\u{10000}/vgd;
    a.lastIndex = 1;
    a = a.exec("\u{10000}_\u{10000}");
    assert(a.indices[0][0], 0);
    assert(a.indices[0][1], 2);

    /* syntax check */
    assert(/-&&/v.test("-&&"), true);
}

function test_symbol()
{
    var a, b, obj, c;
    a = Symbol("abc");
    obj = {};
    obj[a] = 2;
    assert(obj[a], 2);
    assert(typeof obj["abc"], "undefined");
    assert(String(a), "Symbol(abc)");
    b = Symbol("abc");
    assert(a == a);
    assert(a === a);
    assert(a != b);
    assert(a !== b);

    b = Symbol.for("abc");
    c = Symbol.for("abc");
    assert(b === c);
    assert(b !== a);

    assert(Symbol.keyFor(b), "abc");
    assert(Symbol.keyFor(a), undefined);

    a = Symbol("aaa");
    assert(a.valueOf(), a);
    assert(a.toString(), "Symbol(aaa)");

    b = Object(a);
    assert(b.valueOf(), a);
    assert(b.toString(), "Symbol(aaa)");
}

function test_map1(key_type, n)
{
    var a, i, tab, o, v;
    a = new Map();
    tab = [];
    for(i = 0; i < n; i++) {
        v = { };
        switch(key_type) {
        case "small_bigint":
            o = BigInt(i);
            break;
        case "bigint":
            o = BigInt(i) + (1n << 128n);
            break;
        case "object":
            o = { id: i };
            break;
        default:
            assert(false);
        }
        tab[i] = [o, v];
        a.set(o, v);
    }

    assert(a.size, n);
    for(i = 0; i < n; i++) {
        assert(a.get(tab[i][0]), tab[i][1]);
    }

    i = 0;
    a.forEach(function (v, o) {
        assert(o, tab[i++][0]);
        assert(a.has(o));
        assert(a.delete(o));
        assert(!a.has(o));
    });

    assert(a.size, 0);
}

function test_map()
{
    var a, i, n, tab, o, v;
    n = 1000;

    a = new Map();
    for (var i = 0; i < n; i++) {
        a.set(i, i);
    }
    a.set(-2147483648, 1);
    assert(a.get(-2147483648), 1);
    assert(a.get(-2147483647 - 1), 1);
    assert(a.get(-2147483647.5 - 0.5), 1);

    a.set(1n, 1n);
    assert(a.get(1n), 1n);
    assert(a.get(2n**1000n - (2n**1000n - 1n)), 1n);

    test_map1("object", n);
    test_map1("small_bigint", n);
    test_map1("bigint", n);
}

function test_weak_map()
{
    var a, i, n, tab, o, v, n2;
    a = new WeakMap();
    n = 10;
    tab = [];
    for(i = 0; i < n; i++) {
        v = { };
        if (i & 1)
            o = Symbol("x" + i);
        else
            o = { id: i };
        tab[i] = [o, v];
        a.set(o, v);
    }
    o = null;

    n2 = 5;
    for(i = 0; i < n2; i++) {
        a.delete(tab[i][0]);
    }
    for(i = n2; i < n; i++) {
        tab[i][0] = null; /* should remove the object from the WeakMap too */
    }
    std.gc();
    /* the WeakMap should be empty here */
}

function test_weak_map_cycles()
{
    const weak1 = new WeakMap();
    const weak2 = new WeakMap();
    function createCyclicKey() {
        const parent = {};
        const child = {parent};
        parent.child = child;
        return child;
    }
    function testWeakMap() {
        const cyclicKey = createCyclicKey();
        const valueOfCyclicKey = {};
        weak1.set(cyclicKey, valueOfCyclicKey);
        weak2.set(valueOfCyclicKey, 1);
    }
    testWeakMap();
    // Force to free cyclicKey.
    std.gc();
    // Here will cause sigsegv because [cyclicKey] and [valueOfCyclicKey] in [weak1] was free,
    // but weak2's map record was not removed, and it's key refers [valueOfCyclicKey] which is free.
    weak2.get({});
    std.gc();
}

/* 's += x' grows a string held only by 's' in place: the other
   references to it must not see the change. The loops read the string
   between appends, which was O(n^2) when the string was a rope: they
   take seconds if this regresses. */
function test_string_append()
{
    var s, t, w, u, k, o, a, r, i, n = 200000;
    s = "";
    for (i = 0; i < n; i++) {
        s += "ab";
        if (s.charCodeAt(s.length - 1) != 98)
            throw_error("append");
    }
    assert(s.length, 2 * n);
    assert(s.slice(0, 6), "ababab");
    t = s;
    s += "c";
    assert(t.length, 2 * n);
    assert(s.length, 2 * n + 1);
    assert(s.slice(-2), "bc");
    /* starting from a rope */
    s = "x".repeat(10000) + "y".repeat(10000);
    for (i = 0; i < n; i++) {
        s += "z";
        if (s.indexOf("zz", s.length - 3) < 0 && i > 0)
            throw_error("append to a rope");
    }
    assert(s.length, 20000 + n);
    assert(s.slice(9999, 10001), "xy");
    /* 8 bit string receiving a 16 bit character, then 8 bit ones */
    w = "x".repeat(600);
    w += "\u1234";
    for (i = 0; i < n; i++) {
        w += "a";
        if (w.charCodeAt(w.length - 1) != 97)
            throw_error("append after widening");
    }
    assert(w.length, 601 + n);
    assert(w.charCodeAt(600), 0x1234);
    /* 16 bit string + 8 bit string */
    u = "\u1234".repeat(600);
    u += "abc";
    assert(u.length, 603);
    assert(u.slice(-4), "\u1234abc");
    /* the property name of an object is not modified */
    k = "k".repeat(600);
    o = {};
    o[k] = 1;
    k += "z";
    assert(Object.keys(o)[0].length, 600);
    assert(o["k".repeat(600)], 1);
    /* appends which make a rope again between two 's += x' (an element
       or a number on the right side): the rope is not copied by each
       's += x', which was O(n^2), the loops would take seconds. It is
       copied when it was read since the last append. */
    a = ["alpha", "beta", "gamma"];
    r = [];
    s = "";
    for (i = 0; i < n; i++) {
        if (i > 0)
            s += ", ";
        s += a[i % 3];
        r.push(a[i % 3]);
        if (i % 20000 == 19999 && s.charCodeAt(s.length - 1) != 97)
            throw_error("append after a read of a rope");
    }
    assert(s === r.join(", "));
    s = "";
    for (i = 0; i < n; i++) {
        s += i;
        s += ",";
    }
    assert(s.length, 1288890);
    assert(s.slice(-14), "199998,199999,");
    /* a let variable */
    {
        let l = "", m;
        for (i = 0; i < 1000; i++)
            l += "ab";
        m = l;
        l += "c";
        assert(m.length, 2000);
        assert(l.length, 2001);
    }
}

function test_weak_ref()
{
    var w1, w2, o, i;

    for(i = 0; i < 2; i++) {
        if (i == 0)
            o = { };
        else
            o = Symbol("x");
        w1 = new WeakRef(o);
        assert(w1.deref(), o);
        w2 = new WeakRef(o);
        assert(w2.deref(), o);
        
        o = null;
        assert(w1.deref(), undefined);
        assert(w2.deref(), undefined);
        std.gc();
        assert(w1.deref(), undefined);
        assert(w2.deref(), undefined);
    }
}

function test_finalization_registry()
{
    {
        let expected = {};
        let actual;
        let finrec = new FinalizationRegistry(v => { actual = v });
        finrec.register({}, expected);
        os.setTimeout(() => {
            assert(actual, expected);
        }, 0);
    }
    {
        let expected = 42;
        let actual;
        let finrec = new FinalizationRegistry(v => { actual = v });
        finrec.register({}, expected);
        os.setTimeout(() => {
            assert(actual, expected);
        }, 0);
    }
    std.gc();
}

function test_generator()
{
    function *f() {
        var ret;
        yield 1;
        ret = yield 2;
        assert(ret, "next_arg");
        return 3;
    }
    function *f2() {
        yield 1;
        yield 2;
        return "ret_val";
    }
    function *f1() {
        var ret = yield *f2();
        assert(ret, "ret_val");
        return 3;
    }
    function *f3() {
        var ret;
        /* test stack consistency with nip_n to handle yield return +
         * finally clause */
        try {
            ret = 2 + (yield 1);
        } catch(e) {
        } finally {
            ret++;
        }
        return ret;
    }
    var g, v;
    g = f();
    v = g.next();
    assert(v.value === 1 && v.done === false);
    v = g.next();
    assert(v.value === 2 && v.done === false);
    v = g.next("next_arg");
    assert(v.value === 3 && v.done === true);
    v = g.next();
    assert(v.value === undefined && v.done === true);

    g = f1();
    v = g.next();
    assert(v.value === 1 && v.done === false);
    v = g.next();
    assert(v.value === 2 && v.done === false);
    v = g.next();
    assert(v.value === 3 && v.done === true);
    v = g.next();
    assert(v.value === undefined && v.done === true);

    g = f3();
    v = g.next();
    assert(v.value === 1 && v.done === false);
    v = g.next(3);
    assert(v.value === 6 && v.done === true);
}

function rope_concat(n, dir)
{
    var i, s;
    s = "";
    if (dir > 0) {
        for(i = 0; i < n; i++)
            s += String.fromCharCode(i & 0xffff);
    } else {
        for(i = n - 1; i >= 0; i--)
            s = String.fromCharCode(i & 0xffff) + s;
    }
    
    for(i = 0; i < n; i++) {
        /* test before the assert to go faster */
        if (s.charCodeAt(i) != (i & 0xffff)) {
            assert(s.charCodeAt(i), i & 0xffff);
        }
    }
}

function test_rope()
{
    rope_concat(100000, 1);
    rope_concat(100000, -1);
}

/* ropes flow unflattened through the ToPropertyKey of template
   substitutions and computed property names. The loop was O(n^2) when
   the substitution was flattened: it takes seconds if this regresses */
function test_template_rope()
{
    var s, i, k, o, C, f, x, rest, n = 200000, len = 0;
    s = "";
    for (i = 0; i < n; i++) {
        s = `${s}${i}`;
        len += String(i).length;
    }
    assert(s.length, len);
    assert(s.slice(0, 12), "012345678910");
    assert(s.slice(-6), "199999");
    assert(`<${s}>`.length, len + 2);
    assert_throws(TypeError, () => `${Symbol()}`);

    /* ropes as computed keys */
    k = "a".repeat(10000) + "b".repeat(10000);
    o = { [k]: 1, [k + "f"]: function() {} };
    assert(o[k.slice(0)], 1);
    assert(o[k + "f"].name, k + "f");
    C = class { [k] = 2; [k + "m"]() { return 3; } static [k + "s"] = 4; };
    x = new C();
    assert(x[k], 2);
    assert(x[k + "m"](), 3);
    assert(C[k + "s"], 4);
    ({ [k]: f, ...rest } = { [k]: 5, z: 6 });
    assert(f, 5);
    assert(Object.keys(rest).join(), "z");
    class D extends C {
        g() { return super[k + "m"](); }
        h() { super[k + "v"] += 9; return this[k + "v"]; }
    }
    assert(new D().g(), 3);
    C.prototype[k + "v"] = 1;
    assert(new D().h(), 10);

    /* accessors, class names and destructuring to a member */
    o = { get [k + "g"]() { return 7; }, set [k + "g"](v) { f = v; },
          [k + "c"]: class {} };
    o[k + "g"] = 8;
    assert(o[k + "g"], 7);
    assert(f, 8);
    assert(Object.getOwnPropertyDescriptor(o, k + "g").get.name,
           "get " + k + "g");
    assert(o[k + "c"].name, k + "c");
    ({ [k]: o.d = 11, ...rest } = { z: 6 });
    assert(o.d, 11);
    assert(Object.keys(rest).join(), "z");
}

function eval_error(eval_str, expected_error, level)
{
    var err = false;
    var expected_pos, tab;

    tab = get_string_pos(eval_str);
    
    try {
        eval(tab[0]);
    } catch(e) {
        err = true;
        if (!(e instanceof expected_error)) {
            throw_error("unexpected exception type");
            return;
        }
        check_error_pos(e, expected_error, tab[1], tab[2], level);
    }
    if (!err) {
        throw_error("expected exception");
    }
}

var poisoned_number = {
    valueOf: function() { throw Error("poisoned number") },
};

function test_line_column_numbers()
{
    var f, e, tab;

    /* The '@' character provides the expected position of the
       error. It is removed before evaluating the string. */
    
    /* parsing */
    eval_error("\n 123 @a ", SyntaxError);
    eval_error("\n  @/*  ", SyntaxError);
    eval_error("function f  @a", SyntaxError);
    /* currently regexp syntax errors point to the start of the regexp */
    eval_error("\n  @/aaa]/u", SyntaxError); 

    /* function definitions */
    
    tab = get_string_pos("\n   @function f() { }; f;");
    e = eval(tab[0]);
    assert(e.lineNumber, tab[1]);
    assert(e.columnNumber, tab[2]);

    /* errors */
    tab = get_string_pos('\n  Error@("hello");');
    e = eval(tab[0]);
    check_error_pos(e, Error, tab[1], tab[2]);
    
    eval_error('\n  throw Error@("hello");', Error);

    /* operators */
    eval_error('\n  1 + 2 @* poisoned_number;', Error, 1);
    eval_error('\n  1 + "café" @* poisoned_number;', Error, 1);
    eval_error('\n  1 + 2 @** poisoned_number;', Error, 1);
    eval_error('\n  2 * @+ poisoned_number;', Error, 1);
    eval_error('\n  2 * @- poisoned_number;', Error, 1);
    eval_error('\n  2 * @~ poisoned_number;', Error, 1);
    eval_error('\n  2 * @++ poisoned_number;', Error, 1);
    eval_error('\n  2 * @-- poisoned_number;', Error, 1);
    eval_error('\n  2 * poisoned_number @++;', Error, 1);
    eval_error('\n  2 * poisoned_number @--;', Error, 1);

    /* accessors */
    eval_error('\n 1 + null@[0];', TypeError); 
    eval_error('\n 1 + null @. abcd;', TypeError); 
    eval_error('\n 1 + null @( 1234 );', TypeError);
    eval_error('var obj = { get a() { throw Error("test"); } }\n 1 + obj @. a;',
               Error, 1);
    eval_error('var obj = { set a(b) { throw Error("test"); } }\n obj @. a = 1;',
               Error, 1);

    /* variables reference */
    eval_error('\n  1 + @not_def', ReferenceError, 0);

    /* assignments */
    eval_error('1 + (@not_def = 1)', ReferenceError, 0);
    eval_error('1 + (@not_def += 2)', ReferenceError, 0);
    eval_error('var a;\n 1 + (a @+= poisoned_number);', Error, 1);
}

function test_sort_identical_values()
{
    var calls, o = {};
    calls = 0;
    [o, o, o].sort(function(a, b) { calls++; return 0; });
    assert(calls >= 2);
    calls = 0;
    [1, 1].toSorted(function(a, b) { calls++; return 0; });
    assert(calls >= 1);
}

function test_queue_microtask()
{
    var log = [];
    assert(typeof queueMicrotask, "function");
    assert(queueMicrotask.length, 1);
    assert_throws(TypeError, () => queueMicrotask());
    assert_throws(TypeError, () => queueMicrotask({}));
    Promise.resolve().then(() => log.push("p1"));
    queueMicrotask(function() {
        log.push(this === undefined ? "m1" : "bad this");
        queueMicrotask(() => log.push("m3"));
    });
    Promise.resolve().then(() => log.push("p2"));
    assert(queueMicrotask(() => log.push("m2")), undefined);
    log.push("sync");
    os.setTimeout(() => {
        assert(log.join(), "sync,p1,m1,p2,m2,m3");
    }, 0);
}

function test_base64()
{
    assert(btoa(""), "");
    assert(btoa("f"), "Zg==");
    assert(btoa("fo"), "Zm8=");
    assert(btoa("foo"), "Zm9v");
    assert(btoa("\xff\xfe\x00"), "//4A");
    assert(btoa(123), "MTIz");
    assert(atob(""), "");
    assert(atob("Zm9v"), "foo");
    assert(atob("Zg=="), "f");
    assert(atob("Zg"), "f");
    assert(atob(" Zm 9v\n\t\f\r"), "foo");
    assert(atob("Zh=="), "f"); /* non zero trailing bits are ignored */
    assert(atob("//4A"), "\xff\xfe\x00");
    assert(atob(btoa("\x00\x80\xff")), "\x00\x80\xff");
    assert(atob(null), "\x9e\xe9e"); /* "null" */
    for (var s of ["Z", "Zg=", "Zm9v=", "Zg==Zg==", "Z===", "Zm-v", "Zm9\u0100", "Zm9v\u00e9"]) {
        try {
            atob(s);
            throw_error("atob: expected exception for " + JSON.stringify(s));
        } catch (e) {
            assert(e instanceof DOMException, true, s);
            assert(e.name, "InvalidCharacterError");
            assert(e.code, DOMException.INVALID_CHARACTER_ERR);
        }
    }
    try {
        btoa("\u0100");
        throw_error("btoa: expected exception");
    } catch (e) {
        assert(e instanceof DOMException);
        assert(e.code, 5);
    }
    assert_throws(TypeError, () => btoa());
    assert_throws(TypeError, () => atob());
}

function test_dom_exception()
{
    var e, d;
    e = new DOMException();
    assert(e.name, "Error");
    assert(e.message, "");
    assert(e.code, 0);
    assert(e instanceof Error);
    assert(Error.isError(e), true);
    assert(Error.isError(DOMException.prototype), false);
    assert(Object.getPrototypeOf(DOMException.prototype), Error.prototype);
    assert(Object.prototype.toString.call(e), "[object DOMException]");
    assert(String(e), "Error");
    e = new DOMException("msg", "NotFoundError");
    assert(e.name, "NotFoundError");
    assert(e.message, "msg");
    assert(e.code, 8);
    assert(String(e), "NotFoundError: msg");
    assert(typeof e.stack, "string");
    assert(Object.keys(e).length, 0);
    assert(new DOMException("m", "Custom").code, 0);
    assert(new DOMException(undefined, undefined).name, "Error");
    assert(new DOMException(1, 2).name, "2");
    assert(DOMException.length, 0);
    assert(DOMException.name, "DOMException");
    assert_throws(TypeError, () => DOMException());
    assert(DOMException.DATA_CLONE_ERR, 25);
    assert(DOMException.prototype.ABORT_ERR, 20);
    assert(e.TIMEOUT_ERR, 23);
    d = Object.getOwnPropertyDescriptor(DOMException, "INDEX_SIZE_ERR");
    assert(d.value === 1 && d.enumerable && !d.writable && !d.configurable);
    d = Object.getOwnPropertyDescriptor(DOMException.prototype, "name");
    assert(typeof d.get === "function" && d.enumerable && d.configurable && d.set === undefined);
    d = Object.getOwnPropertyDescriptor(globalThis, "DOMException");
    assert(d.writable && !d.enumerable && d.configurable);
    assert_throws(TypeError, () => DOMException.prototype.name);
    assert_throws(TypeError, () => Object.getOwnPropertyDescriptor(DOMException.prototype, "code").get.call({}));
    class MyError extends DOMException {
        constructor() { super("sub", "AbortError"); }
    }
    e = new MyError();
    assert(e instanceof MyError && e instanceof DOMException);
    assert(e.name, "AbortError");
    assert(e.code, 20);
}

/* the Two-Way search (long needles and haystacks) against a naive search,
   with periodic needles and all the combinations of string widths */
function test_string_search()
{
    var seed = 1, iter, h, n, from, i, k, exp, parts, p, e;
    var alphabets = [ ["a", "b"], ["a", "b", "c"], ["a"], ["\u0100", "a"],
                      ["\uffff", "\u00ff", "a"], ["\ud83d", "\ude00", "x"] ];
    function rnd(n) {
        seed = (seed * 1103515245 + 12345) & 0x7fffffff;
        return seed % n;
    }
    function rstr(alpha, len) {
        var s = "";
        while (s.length < len)
            s += alpha[rnd(alpha.length)];
        return s;
    }
    function periodic(alpha, len) {
        var unit = rstr(alpha, 1 + rnd(4)), s = "";
        while (s.length < len)
            s += unit;
        return s.slice(0, len) + rstr(alpha, rnd(3));
    }
    function naive_index(h, n, from, step) {
        for (i = from; i >= 0 && i + n.length <= h.length; i += step) {
            for (k = 0; k < n.length && h.charCodeAt(i + k) === n.charCodeAt(k); k++)
                continue;
            if (k === n.length)
                return i;
        }
        return -1;
    }
    for (iter = 0; iter < 4000; iter++) {
        var alpha = alphabets[rnd(alphabets.length)];
        h = rnd(3) ? rstr(alpha, 60 + rnd(200)) : periodic(alpha, 60 + rnd(200));
        switch (rnd(3)) {
        case 0:
            n = periodic(alpha, 10 + rnd(30));
            break;
        case 1:
            p = rnd(h.length);
            n = h.slice(p, p + 10 + rnd(40)) + (rnd(2) ? alpha[rnd(alpha.length)] : "");
            break;
        default:
            n = rstr(alphabets[rnd(alphabets.length)], 5 + rnd(5));
            break;
        }
        from = rnd(h.length);
        assert(h.indexOf(n, from), naive_index(h, n, from, 1));
        assert(h.includes(n, from), naive_index(h, n, from, 1) >= 0);
        exp = naive_index(h, n, Math.min(from, h.length - n.length), -1);
        assert(h.lastIndexOf(n, from), exp);
        parts = [];
        for (p = 0; (e = naive_index(h, n, p, 1)) >= 0; p = e + n.length)
            parts.push(h.slice(p, e));
        parts.push(h.slice(p));
        assert(h.split(n).join("|"), parts.join("|"));
    }
    /* worst case of the naive search */
    h = "a".repeat(200000);
    n = "a".repeat(5000) + "b";
    assert(h.indexOf(n), -1);
    assert((h + "b").indexOf(n), 200000 - 5000);
    assert(("b" + h).lastIndexOf("b" + "a".repeat(5000)), 0);
    assert(h.includes(n), false);
}

/* shift() leaves free slots before the elements of a fast array: they
   must be reused or kept through the other array mutations */
function test_array_shift()
{
    var a = [], ref = [], seed = 5, it, op, v, i, s, n;
    function rnd(n) {
        seed = (seed * 1103515245 + 12345) & 0x7fffffff;
        return seed % n;
    }
    function check() {
        assert(a.length, ref.length);
        for (i = 0; i < ref.length; i++)
            assert(a[i], ref[i]);
    }
    for (it = 0; it < 3000; it++) {
        op = rnd(8);
        v = it;
        switch (op) {
        case 0: case 1: case 2:
            a.push(v);
            ref[ref.length] = v;
            break;
        case 3: case 4:
            assert(a.shift(), ref.length ? ref[0] : undefined);
            ref = ref.slice(1);
            break;
        case 5:
            a.unshift(v);
            ref = [v].concat(ref);
            break;
        case 6:
            s = rnd(ref.length + 1);
            n = rnd(3);
            a.splice(s, n, v);
            ref = ref.slice(0, s).concat([v], ref.slice(s + n));
            break;
        case 7:
            if (rnd(10) == 0) {
                n = rnd(ref.length + 1);
                a.length = n;
                ref.length = n;
            }
            break;
        }
        if ((it & 7) == 0)
            check();
    }
    check();
    /* queue at a constant size: the free slots are reused */
    a = [];
    for (i = 0; i < 1000; i++)
        a.push(i);
    for (i = 0; i < 10000; i++)
        a.push(a.shift());
    assert(a.length, 1000);
    for (i = 0; i < 1000; i++)
        assert(a[i], i);
    /* drained to empty then refilled */
    while (a.length)
        a.shift();
    a.push(1, 2, 3);
    assert(a.join(), "1,2,3");
}

test();
test_function();
test_enum();
test_array();
test_array_shift();
test_string();
test_string_search();
test_math();
test_number();
test_eval();
test_typed_array();
test_immutable_array_buffer();
test_json();
test_duplicate_keys();
test_date();
test_regexp();
test_symbol();
test_map();
test_weak_map();
test_weak_map_cycles();
test_string_append();
test_weak_ref();
test_finalization_registry();
test_generator();
test_rope();
test_template_rope();
test_line_column_numbers();
test_sort_identical_values();
test_queue_microtask();
test_base64();
test_dom_exception();
