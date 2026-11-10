// Standard ECMAScript decorators and auto-accessors
import { assert, assertThrows } from "./assert.js";

function test_accessor() {
    const sym = Symbol("s");
    const key = "k";
    class C {
        accessor a;
        accessor b = 1;
        accessor "c" = 2;
        accessor [key] = 3;
        accessor [sym] = 4;
        accessor 5 = 5;
        static accessor s = 6;
        accessor #p = 7;
        static accessor #sp = 8;
        getP() { return this.#p; }
        setP(v) { this.#p = v; }
        static getSP() { return C.#sp; }
        static hasP(o) { return #p in o; }
    }
    const c = new C();
    assert(c.a, undefined);
    assert(c.b + c.c + c.k + c[sym] + c[5] + C.s, 21);
    c.b = 10;
    assert(c.b, 10);
    assert(Object.hasOwn(c, "b"), false, "the storage is private");
    const desc = Object.getOwnPropertyDescriptor(C.prototype, "b");
    assert(typeof desc.get + typeof desc.set, "functionfunction");
    assert(desc.enumerable, false);
    assert(desc.get.name + "," + desc.set.name, "get b,set b");
    assert(c.getP(), 7);
    c.setP(70);
    assert(c.getP(), 70);
    assert(C.getSP(), 8);
    assert(C.hasP(c), true);
    assert(C.hasP({}), false);
    /* the getter checks the instance */
    assertThrows(TypeError, () => desc.get.call({}));
    /* 'accessor' is still an identifier */
    class D {
        accessor;
        accessor
        x = 1;
        accessor() { return 2; }
    }
    assert(new D().x, 1);
    assert(Object.hasOwn(new D(), "accessor"), true);
    /* duplicate private names */
    assertThrows(SyntaxError, () => eval("class E { accessor #x; #x; }"));
    assertThrows(SyntaxError, () => eval("class E { accessor #x; get #x() {} }"));
    assertThrows(SyntaxError, () => eval("class E { accessor m() {} }"));
}

function test_order() {
    const log = [];
    function dec(name) {
        log.push("eval " + name);
        return function (value, ctx) {
            log.push(name);
            ctx.addInitializer(function () { log.push("init " + name); });
            if (ctx.kind === "field")
                return (v) => { log.push("field init " + name); return v; };
            if (ctx.kind === "accessor")
                return { init(v) { log.push("accessor init " + name); return v; } };
        };
    }
    log.push("start");
    @dec("c1") @dec("c2")
    class C {
        @dec("f") f = log.push("f value");
        @dec("sf") static sf = log.push("sf value");
        @dec("m") m() {}
        @dec("sm") static sm() {}
        @dec("a") accessor a;
        @dec("sa") static accessor sa;
        static { log.push("static block"); }
        constructor() { log.push("ctor"); }
    }
    log.push("defined");
    new C();
    assert(log.join(),
           "start,eval c1,eval c2,eval f,eval sf,eval m,eval sm,eval a,eval sa," +
           /* static non-fields, instance non-fields, static fields,
              instance fields, class */
           "sm,sa,m,a,sf,f,c2,c1," +
           /* static method initializers, static elements, class initializers */
           "init sm,sf value,field init sf,init sf,accessor init sa,init sa," +
           "static block,init c2,init c1,defined," +
           /* instance method initializers, fields */
           "init m,f value,field init f,init f,accessor init a,init a,ctor");

    /* the decorators are applied from the last one, the initializers of
       the first decorator run first */
    const order = [];
    const d = (n) => () => (v) => { order.push(n); return v + n; };
    class D { @d("a") @d("b") x = ""; }
    assert(new D().x, "ab");
    assert(order.join(), "a,b");
}

function test_context() {
    const ctxs = {};
    const save = (value, ctx) => { ctxs[String(ctx.name)] = { value, ctx }; };
    @save class C {
        @save m() { return "m"; }
        @save static sm() {}
        @save get g() { return this.v; }
        @save set s(v) { this.v = v; }
        @save f = 1;
        @save accessor a = 2;
        @save #pm() { return "pm"; }
        @save get #pg() { return "pg"; }
        @save set #ps(v) { this.pv = v; }
        @save #pf = 3;
        @save accessor #pa = 4;
        static test(o) { return [o.#pm(), o.#pg, o.#pf, o.#pa]; }
    }
    const c = new C();
    let ctx = ctxs.C.ctx;
    assert(ctx.kind, "class");
    assert(ctxs.C.value, C);
    assert("static" in ctx || "private" in ctx || "access" in ctx, false);
    assert(typeof ctx.addInitializer, "function");
    ctx = ctxs.m.ctx;
    assert([ctx.kind, ctx.name, ctx.static, ctx.private].join(), "method,m,false,false");
    assert(ctxs.m.value, C.prototype.m);
    assert(ctx.access.get(c), C.prototype.m);
    assert(ctx.access.has(c) && !ctx.access.has({}), true);
    assert("set" in ctx.access, false);
    assertThrows(TypeError, () => ctx.access.get(1));
    assert(ctxs.sm.ctx.static, true);
    assert(ctxs.g.ctx.kind + ctxs.s.ctx.kind, "gettersetter");
    ctxs.s.ctx.access.set(c, 5);
    assert(ctxs.g.ctx.access.get(c), 5);
    assert("get" in ctxs.s.ctx.access, false);
    ctx = ctxs.f.ctx;
    assert(ctx.kind, "field");
    assert(ctxs.f.value, undefined);
    assert(ctx.access.get(c), 1);
    ctx.access.set(c, 10);
    assert(c.f, 10);
    ctx = ctxs.a.ctx;
    assert(ctx.kind, "accessor");
    assert(typeof ctxs.a.value.get + typeof ctxs.a.value.set, "functionfunction");
    assert(ctx.access.get(c), 2);
    /* private elements */
    ctx = ctxs["#pm"].ctx;
    assert([ctx.kind, ctx.name, ctx.private].join(), "method,#pm,true");
    assert(ctx.access.get(c)(), "pm");
    assert(ctx.access.has(c) && !ctx.access.has({}), true);
    assertThrows(TypeError, () => ctx.access.get({}));
    assert(ctxs["#pg"].value.name, "get #pg");
    assert(ctxs["#pg"].ctx.access.get(c), "pg");
    assert(ctxs["#ps"].value.name, "set #ps");
    ctxs["#ps"].ctx.access.set(c, 6);
    assert(c.pv, 6);
    assert(ctxs["#pf"].ctx.access.get(c), 3);
    ctxs["#pf"].ctx.access.set(c, 30);
    assert(ctxs["#pa"].ctx.access.get(c), 4);
    ctxs["#pa"].ctx.access.set(c, 40);
    assert(C.test(c).join(), "pm,pg,30,40");
    /* the metadata object is shared */
    assert(ctxs.m.ctx.metadata, ctxs.C.ctx.metadata);
    assert(C[Symbol.metadata], ctxs.C.ctx.metadata);
    /* the receiver of '@a.b' is 'a' */
    let recv;
    const obj = { dec() { recv = this; } };
    class D { @obj.dec m() {} }
    assert(recv, obj);
    /* computed keys and symbols */
    const sym = Symbol("sym");
    const names = [];
    const n = (v, ctx) => { names.push(ctx.name); };
    class E { @n [sym]() {} @n ["a" + 1]() {} @n [2] = 1; @n accessor [String(sym)] }
    assert(names[0], sym);
    assert(names.slice(1).join(), "a1,Symbol(sym),2");
    /* string ropes as computed keys */
    const rope = "a".repeat(600) + "b".repeat(600);
    names.length = 0;
    class F { @n [rope]() {} @n accessor [rope + 1] = 1; accessor [rope + 2] = 2; }
    assert(names.join(), rope + "," + rope + 1);
    const f = new F();
    assert(f[rope + 1], 1);
    assert(f[rope + 2], 2);
}

function test_replacement() {
    const wrap = (m, ctx) => function (...args) { return "<" + m.call(this, ...args) + ">"; };
    const twice = (m, ctx) => function (v) { return m.call(this, v * 2); };
    class C {
        @wrap m(x) { return x; }
        @wrap static sm() { return "s"; }
        @wrap get g() { return "g"; }
        set s(v) { this.v = v; }
        @twice set s(v) { this.v = v; }
        @wrap #pm() { return "pm"; }
        @wrap get #pg() { return "pg"; }
        @twice set #ps(v) { this.pv = v; }
        @wrap *gen() { yield 1; }
        test() {
            this.#ps = 2;
            return [this.#pm(), this.#pg, this.pv, #pm in this, this.#pm === this.#pm].join();
        }
    }
    const c = new C();
    assert(c.m(1), "<1>");
    assert(C.sm(), "<s>");
    assert(c.g, "<g>");
    c.s = 2;
    assert(c.v, 4);
    assert(c.test(), "<pm>,<pg>,4,true,true");
    assertThrows(TypeError, () => C.prototype.test.call({}));
    /* auto-accessor: get, set and init */
    const acc = ({ get, set }, ctx) => ({
        get() { return get.call(this) * 10; },
        set(v) { set.call(this, v + 1); },
        init(v) { return v + 100; },
    });
    class D {
        @acc accessor a = 1;
        @acc accessor #b = 2;
        @acc static accessor c = 3;
        getB() { return this.#b; }
        setB(v) { this.#b = v; }
    }
    const d = new D();
    assert(d.a, 1010);
    d.a = 1;
    assert(d.a, 20);
    assert(d.getB(), 1020);
    d.setB(5);
    assert(d.getB(), 60);
    assert(D.c, 1030);
    /* class replacement */
    let orig;
    const replace = (cls, ctx) => {
        orig = cls;
        return class extends cls { static replaced = true; };
    };
    @replace class E {
        static self = E;
        static that = this;
        static getE() { return E; }
    }
    assert(E.replaced, true);
    assert(Object.getPrototypeOf(E), orig);
    assert(E.getE(), E, "the class binding is the new class");
    assert(E.self, E);
    assert(E.that, E, "the static fields are defined on the new class");
    const F = @replace class {};
    assert(F.replaced && orig.name === "F", true);
}

function test_errors() {
    const ret = (v) => () => v;
    assertThrows(TypeError, () => { class C { @(ret(1)) m() {} } });
    assertThrows(TypeError, () => { class C { @(ret({})) f; } });
    assertThrows(TypeError, () => { class C { @(ret(1)) accessor a; } });
    assertThrows(TypeError, () => { class C { @(ret({ get: 1 })) accessor a; } });
    assertThrows(TypeError, () => { @(ret(null)) class C {} });
    let saved;
    class D { @((v, ctx) => { saved = ctx; }) m() {} }
    assertThrows(TypeError, () => saved.addInitializer(() => {}));
    assertThrows(TypeError, () => { class C { @((v, ctx) => { ctx.addInitializer(1); }) m() {} } });
    /* exceptions from the decorators */
    const thrower = () => { throw new RangeError(); };
    assertThrows(RangeError, () => { @thrower class C {} });
    assertThrows(RangeError, () => { class C { @thrower static m() {} } });
    /* syntax */
    assertThrows(SyntaxError, () => eval("class C { @dec static { } }"));
    assertThrows(SyntaxError, () => eval("class C { @dec constructor() {} }"));
    assertThrows(SyntaxError, () => eval("@dec function f() {}"));
    assertThrows(SyntaxError, () => eval("@dec let x;"));
    assertThrows(SyntaxError, () => eval("class C { @dec[0] m() {} }"));
    assertThrows(SyntaxError, () => eval("class C { m(@dec x) {} }"));
    /* decorators are evaluated in the class scope, the class binding is
       in TDZ */
    assertThrows(ReferenceError, () => { class C { @(C) m() {} } });
}

function test_metadata() {
    const meta = (k, v) => (_, ctx) => { ctx.metadata[k] = v; };
    @meta("a", 1) class A {
        @meta("m", 2) m() {}
    }
    class B extends A {
        @meta("b", 3) #f;
    }
    class C extends B {}
    class D {}
    const ma = A[Symbol.metadata], mb = B[Symbol.metadata];
    assert(Object.getPrototypeOf(ma), null);
    assert(ma.a + ma.m, 3);
    assert(Object.getPrototypeOf(mb), ma);
    assert(mb.b + mb.a, 4);
    assert(Object.hasOwn(mb, "a"), false);
    assert(C[Symbol.metadata], mb, "inherited through the class");
    assert(D[Symbol.metadata], undefined);
    assert(typeof Symbol.metadata, "symbol");
    class E extends null { @meta("e", 1) static m() {} }
    assert(Object.getPrototypeOf(E[Symbol.metadata]), null);
}

function test_misc() {
    /* named evaluation of anonymous classes */
    let name;
    const getName = (cls, ctx) => { name = ctx.name + "/" + cls.name; };
    const X = @getName class {};
    assert(name, "X/X");
    const obj = { Y: @getName class {} };
    assert(name, "Y/Y");
    @getName class Z {}
    assert(name, "Z/Z");
    /* 'this' and decorators in methods and nested classes */
    const o = {
        tag: "o",
        make() {
            const tag = (v, ctx) => { ctx.addInitializer(function () { this.tag = o.tag; }); };
            return class { @tag m() {} };
        },
    };
    const K = o.make();
    assert(new K().tag, "o");
    class Outer {
        static #count = 0;
        static inc = (v, ctx) => { Outer.#count++; };
        static make() {
            return class { @Outer.inc m() {} @Outer.inc n() {} };
        }
        static get count() { return Outer.#count; }
    }
    Outer.make();
    assert(Outer.count, 2);
    /* decorated private methods with direct eval */
    const w = (m) => function () { return "w" + m.call(this); };
    class P {
        @w #m() { return 1; }
        run(src) { return eval(src); }
    }
    assert(new P().run("this.#m()"), "w1");
    /* generators and async methods keep their kind */
    const id = (m) => m;
    class G {
        @id *gen() { yield 1; yield 2; }
        @id async am() { return 3; }
    }
    assert([...new G().gen()].join(), "1,2");
    assert(new G().am() instanceof Promise, true);
}

test_accessor();
test_order();
test_context();
test_replacement();
test_errors();
test_metadata();
test_misc();
