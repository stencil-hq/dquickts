# rquickjs

[![github](https://img.shields.io/badge/github-stencil--hq/dquickts-8da0cb.svg?style=for-the-badge&logo=github)](https://github.com/stencil-hq/dquickts)

This library is a high level bindings of the [QuickJS](https://bellard.org/quickjs/) Javascript engine.
Its goal is to be an easy to use, and safe wrapper similar to the rlua library.

## This copy

Vendored from [rquickjs](https://github.com/DelSkayn/rquickjs) master `85517b7` (0.14.0) into the
[dquickts](../README.md) QuickJS tree two directories above, which `rquickjs-sys` compiles
(bellard's QuickJS with heap snapshots and suspension, instead of the QuickJS-NG submodule of
upstream rquickjs). The crates are not published.

Added:

- Suspension: `Runtime::suspender()` returns a `Suspender` (usable from any thread or from the
  interrupt handler) whose `request()` suspends the running JavaScript at its next suspendable
  point; the call which started it returns `Error::Suspended` and `Ctx::resume()` continues it.
- Heap snapshots (`rquickjs::snapshot`): `Ctx::write_snapshot()` saves the whole runtime
  (including a suspended execution) as a full, base or delta snapshot; `Runtime::read_snapshot()`
  rebuilds it in another runtime or process. A `SnapshotHost` registers the Rust natives and
  classes (`Persist`) a snapshot can reference.
- Optional script builtins (`SnapshotHost::builtins()`): `snapshot()` / `restore()`, like
  `setjmp()` / `longjmp()`, with a `Uint8Array` or a file.
- TypeScript: the engine erases types while parsing (`EvalOptions::typescript`, or files and
  modules named `*.ts`, `*.mts`, `*.cts`); enums, namespaces and parameter properties compile to
  bytecode, `const enum` members are folded; standard or legacy decorators
  (`Runtime::set_decorators`), `using` declarations.
- Python 3.15 (cargo feature `python`, off by default): the engine's embedded Python runtime is
  compiled in, see [Python](#python).


Removed (QuickJS-NG only): `Value::is_uncatchable_error` (see `Ctx::has_uncatchable_exception`).
The bindings are generated from `quickjs.h` by `scripts/gen-bindings.sh`;
`wasm32-unknown-unknown` is not supported.

**QuickJS** is a small and embeddable JavaScript engine. It supports the _ES2020_ specification including modules, asynchronous generators, proxies and BigInt.
It optionally supports mathematical extensions such as big decimal floating point numbers (BigDecimal), big binary floating point numbers (BigFloat) and operator overloading.

## Python

With the `python` feature, `rquickjs-sys` compiles the engine's Python 3.15 runtime
(`CONFIG_PYTHON`: `python/*.c`, included by `quickjs.c` as in the engine Makefile). Python and
JavaScript share one heap and their values by identity (`list` = `Array`, `str` = `String`, `int`
= number (int32) or `BigInt`, `float` = number, `None` = `undefined`, functions are functions,
`dict` keys read as properties), so results convert with the ordinary `FromJs` / `IntoJs`:

```rust
use rquickjs::{context::{PythonEvalMode, PythonEvalOptions}, Context, Function, Object, Promise, Runtime};

let rt = Runtime::new()?;
let ctx = Context::full(&rt)?;
ctx.with(|ctx| -> rquickjs::Result<()> {
    let squares: Vec<i32> = ctx.eval_python("[x * x for x in range(4)]")?;

    let mut exec = PythonEvalOptions::default();
    exec.mode = PythonEvalMode::Exec;
    ctx.eval_python_with_options::<(), _>("async def double(x):\n    return x * 2\n", exec)?;
    let double: Function = ctx.eval_python("double")?;

    // top-level await: a Promise, settled by the job queue
    let mut tla = PythonEvalOptions::default();
    tla.promise = true;
    let promise: Promise = ctx.eval_python_with_options("await double(21)", tla)?;
    assert_eq!(promise.finish::<i32>()?, 42);

    let json: Object = ctx.python_import("json")?;
    let dumps: Function = json.get("dumps")?;
    assert_eq!(dumps.call::<_, String>((squares,))?, "[0, 1, 4, 9]");
    Ok(())
})?;
```

- `Ctx::init_python(PythonConfig)`: `sys.argv`, `sys.executable`, the Python home (the
  standard library is `<home>/pylib`), `sys.path[0]`, a `PYTHONPATH`-style list, the hash
  seed, `-O`, `-I` and the recursion limit. Optional: the first `eval_python` / `python_import`
  initializes Python with the defaults, and a later configuration is ignored.
- `Ctx::eval_python` evaluates an expression in the globals of `__main__`;
  `Ctx::eval_python_with_options` takes `PythonEvalOptions` (mode exec / eval / single, the
  filename, top-level `await` which returns a `Promise`).
- `Ctx::python_import(name)`: `importlib.import_module`, the module object.
- A Python exception is `Error::Exception`; `Ctx::catch()` returns the Python exception object
  (its attributes are properties).
- The standard library: `QPYHOME` at run time, else the `pylib` directory compiled in
  (`QP_PYLIB_DIR`): the `pylib` of the QuickJS tree, or the directory named by
  `RQUICKJS_PYLIB_DIR` when `rquickjs-sys` is built. Deploying elsewhere: set
  `PythonConfig::home` (or `QPYHOME`) to a directory containing `pylib`.
- Without a module loader (`Runtime::set_loader`), Python installs one which imports `.py`
  specifiers of ES modules as Python modules.
- C modules (64-bit Unix targets): the Python C API and CPython's self-contained C modules
  (`_sre`, `_json`, `itertools`, `_collections`, `math`, `_decimal`, `_datetime`, the hash
  modules, `pyexpat`, `_elementtree`, `_socket`, the CJK codecs, ...; `capi/Setup.qp`) are
  compiled in with `python`. The modules that link a system library have a feature each,
  off by default: `python-zlib`, `python-bz2`, `python-lzma`, `python-sqlite3`, `python-ssl`
  (`_ssl`, `_hashlib`), `python-zstd`, `python-ctypes` (libffi), `python-readline` (GNU
  readline or libedit) and `python-uuid` (libuuid; macOS uses libSystem without it). The
  libraries are found with pkg-config, else linked by name. On macOS `_scproxy` links the
  SystemConfiguration and CoreFoundation frameworks.
- The CPython C modules (the ones above and `dlopen`ed extensions) are bound to one runtime of
  the process at a time: the first context that imports one owns them, and other runtimes, in
  any thread, get `ImportError` for them until that runtime is dropped; a later runtime then
  binds them again. Dropping the `Runtime` finalizes Python (`atexit`, module finalization) and
  frees its C extension state. The crate's own Python tests take turns on a mutex for this
  reason, since `cargo test` runs tests on parallel threads.

## Main features of QuickJS

- Small and easily embeddable: just a few C files, no external dependency, 210 KiB of x86 code for a simple hello world program.
- Fast interpreter with very low startup time: runs the 75000 tests of the ECMAScript Test Suite in about 100 seconds on a single core of a desktop PC.
  The complete life cycle of a runtime instance completes in less than 300 microseconds.
- Almost complete ES2020 support including modules, asynchronous generators and full Annex B support (legacy web compatibility).
- Passes nearly 100% of the ECMAScript Test Suite tests when selecting the ES2020 features. A summary is available at Test262 Report.
- Can compile JavaScript sources to executables with no external dependency.
- Garbage collection using reference counting (to reduce memory usage and have deterministic behavior) with cycle removal.
- Mathematical extensions: BigDecimal, BigFloat, operator overloading, bigint mode, math mode.
- Command line interpreter with contextual colorization implemented in JavaScript.
- Small built-in standard library with C library wrappers.

## Features provided by this crate

- Full integration with async Rust
  - The ES6 Promises can be handled as Rust futures and vice versa
  - Easy integration with almost any async runtime or executor
- Flexible data conversion between Rust and JS
  - Many widely used Rust types can be converted to JS and vice versa
- Support for user-defined allocators
  - The `Runtime` can be created using custom allocator
  - Using Rust's global allocator is also fully supported
- Support for user-defined module resolvers and loaders which also
  can be combined to get more flexible solution for concrete case
- Support for bundling JS modules as a bytecode using `embed` macro
- Support for deferred calling of JS functions
- Full support of ES6 classes
  - Rust data types can be represented as JS classes
  - Data fields can be accessed via object properties
  - Both static and instance members is also supported
  - The properties can be defined with getters and setters
  - Support for constant static properties
  - Support for holding references to JS objects
    (Data type which holds refs should implement `Trace` trait to get garbage collector works properly)
  - Support for extending defined classes by JS

## Community development

This crate doesn't aim to provide system and web APIs. The QuickJS library is close to [V8](https://v8.dev/) in that regard.
If you need APIs from [WinterGC](https://wintercg.org/) or [Node](https://nodejs.org/api/), then you can take a look at the follow community projects:

- [AWS LLRT Modules](https://github.com/awslabs/llrt/tree/main/llrt_modules): Collection of modules that micmic some of the `Node` APIs in pure Rust
- [Rquickjs Extra](https://github.com/rquickjs/rquickjs-extra): Collection of modules that complement `AWS LLRT Modules` in pure Rust

The community has also built various utilities which might be relevant to you:

- [Rquickjs Serde](https://github.com/rquickjs/rquickjs-serde): Serde serializer and deserializer for rquickjs Value

## Development status

This bindings is feature complete, mostly stable and ready to use.
The error handling is only thing which may change in the future.
Some experimental features like `parallel` may not works as expected. Use it for your own risk.

## Supported platforms

Rquickjs needs to compile a C-library which has it's own limitation on supported platforms, furthermore it needs to generate bindings for that platform.
As a result rquickjs might not compile on all platforms which rust supports.
In general you can allways try to compile rquickjs with the `bindgen` feature, this should work for most platforms.
Rquickjs ships bindings for a limited set of platforms, for these platforms you don't have to enable the `bindgen` feature.
See below for the platforms of this copy ("builds": the engine compiles with `cargo zigbuild`).

| **platform**                   | **shipped bindings** | **builds** | **tested** |
| ------------------------------ | :------------------: | :--------: | :--------: |
| aarch64-apple-darwin           |          ✅          |     ✅     |     ✅     |
| x86_64-apple-darwin            |          ✅          |     ❓     |     ❌     |
| x86_64-unknown-linux-gnu       |          ✅          |     ✅     |     ❌     |
| aarch64-unknown-linux-gnu      |          ✅          |     ❓     |     ❌     |
| i686-unknown-linux-gnu         |          ✅          |     ✅     |     ❌     |
| armv7-unknown-linux-gnueabihf  |          ✅          |     ✅     |     ❌     |
| riscv64gc-unknown-linux-gnu    |          ✅          |     ✅     |     ❌     |
| x86_64-pc-windows-gnu          |          ✅          |     ✅     |     ❌     |
| \*-musl, loongarch64, msvc     |          ✅          |     ❓     |     ❌     |
| wasm32-wasip1, wasm32-wasip2   |          ✅          |  ❌ (setjmp) |     ❌     |
| other                          |          ❌          |     ❓     |     ❌     |

## License

This library is licensed under the [MIT License](LICENSE)
