# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## Unreleased

### Breaking Changes

- Built on the bellard QuickJS tree this copy is vendored in instead of QuickJS-NG: `Value::is_uncatchable_error` is removed (see `Ctx::has_uncatchable_exception`); `Runtime::set_dump_flags` takes the `JS_DUMP_*` flags of this engine, compiled in by any `dump-*` feature.
- `Allocator` implementations are given the memory limit through QuickJS's allocation state: `Runtime::set_memory_limit` also applies with a custom allocator.

### Added

- `Runtime::suspender` / `Suspender`, `Error::Suspended`, `Ctx::resume`, `Ctx::resume_with` and `Runtime::is_suspended`: suspend a running execution and continue it later.
- `rquickjs::snapshot`: `Ctx::write_snapshot` (full, base and delta snapshots), `Runtime::read_snapshot`, `Runtime::drop_snapshot_base`, `SnapshotHost` (natives and `Persist` classes) and the optional `snapshot()`/`restore()` builtins with `Error::Restore`.
- `Ctx::has_uncatchable_exception`.
- `ArrayBuffer::is_immutable`.
- `StringNormalize` and `AToB` (`atob()`/`btoa()`) intrinsics.
- TypeScript: `EvalOptions::typescript`; files and modules named `*.ts`, `*.mts`, `*.cts` are TypeScript (`Ctx::eval_file`, `Module::declare`/`evaluate`, `ScriptLoader`); `FileResolver` resolves `./x.js` to an existing `./x.ts` and tries `{}.ts`/`{}.mts`; `Runtime::set_decorators` (`Decorators`: standard, TypeScript legacy, legacy with `design:*` metadata).
- `#[rquickjs::methods]` accepts `enumerable`, `configurable`, and `writable` on functions, and `= false` to disable default attributes
- Added `Value::loose_eq`, `Value::strict_eq`, `Value::same_value`, and `Value::same_value_zero` which map to the various concepts of equality exposed by QuickJS.
- `Runtime::execute_pending_jobs` runs the pending jobs in one call (up to a limit), stopping at the first job that throws.
- `String::to_str` borrows the contents of an ASCII string without copying.
- `ObjectBuilder` builds a JS object from keys and values in one step, with the same result as `Object::new` followed by `set` calls.
- Python 3.15 (feature `python`): `rquickjs-sys` compiles the engine's Python runtime (`CONFIG_PYTHON`, stdlib directory `QP_PYLIB_DIR` = the tree's `pylib` or `RQUICKJS_PYLIB_DIR`) and binds `JS_InitPython`, `JS_EvalPython`, `JS_PythonImport` (`JSPythonConfig`, `JS_PY_EVAL_*`); `Ctx::init_python` (`PythonConfig`), `Ctx::eval_python`, `Ctx::eval_python_with_options` (`PythonEvalOptions`, `PythonEvalMode`, top-level `await` returning a `Promise`) and `Ctx::python_import`. Python values are JS values, Python exceptions are `Error::Exception` with the exception object in `Ctx::catch`.

### Changed

- Updated repository references and internal documentation links for the dquickts engine
- Faster bindings, using new engine APIs (inline context reference counting, `JS_GetFastArray`, `JS_GetStringData`, `JS_GetTypedArrayData`, `JS_GetPropertyStrLen`/`JS_SetPropertyStrLen`, `JS_NewObjectFrom` with cached shapes, lazy error stacks): calling a Rust function from JS went from 46 to 12 ns, a class method from 46 to 10 ns, `Function::call` from 34 to 13 ns, `Object::get` with a `&str` key from 12 to 3.4 ns, `Vec<i32>` (100) to JS from 1000 to 200 ns and back from 1100 to 230 ns, a 1 KB Latin-1 or UTF-16 string to Rust from about 1 µs to 60–120 ns, and throwing an error from Rust from 277 to 44 ns (macOS arm64).
- rquickjs sets the opaque of the contexts it creates (`JS_SetContextOpaque`); embedders must not replace it.
- Rust functions are called without dynamic dispatch, and `&self`/`&mut self` methods and `&T` class arguments borrow the class cell only for the call.
- Length-tracking typed arrays (on resizable buffers) expose the current length of the buffer in `as_bytes`/`as_raw`/`as_slice`/`len`, instead of the initial one.
- `Object::len`/`is_empty` return 0/`true` when the enumeration throws (the error was counted as one key).
- Error messages thrown from Rust are no longer cut at 255 bytes or at a NUL character.

### Deprecated

### Removed

### Fixed

- Fixed `Ctx::eval` and other related methods returning a `String contained internal null bytes` error when JS source contains null bytes
- Fixed `#[rquickjs::methods]` failing to compile a setter with no matching getter
- Fixed `Promise::result` and `Promise::into_future` leaving an already rejected promise unhandled, which made the host promise rejection tracker report a false unhandled rejection
- Fixed exotic classes freeing the value passed to `exotic_set_property` twice
- `Atom::to_value` returns a symbol if the atom represents a symbol, instead of always returning a JS string
- `CString::from_string` and `Atom::to_string` returned strings with invalid UTF-8 (undefined behaviour) for JS strings containing unpaired surrogates; they now return `Error::Utf8` like `String::to_string`.
- `Function::call` with more than 4 arguments could panic when the argument count was under-estimated.
- `TypedArray::len` no longer runs a user-defined `length` getter.
- Checking whether a value is a class instance no longer clears an exception that was already pending.

### Security

## [0.14.0] - 2026-09-17

### Breaking Changes

- Removed unused trait `StaticJsFunction`.

### Added

- Added `Exception::throw_dom`, `PredefinedAtom::DOMException`, and `intrinsic::DOMException`.
- Support for `wasm32-unknown-unknown` from [@mrchantey]

### Fixed

- Fixed `Params::is_constructor` to return the suggested value instead of `false`.
- Fixed `ExoticMethod` to fix type checking for `&Ctx`
- Fixed `Ctx::execute_pending_job` treating a job that threw the same as one that ran successfully, leaving the exception pending on the runtime instead of clearing it.
- Fixed `Object::instance_of` leaving a pending exception on the runtime when the checked object isn't an instance of the given Rust class.

## [0.13.0] - 2026-09-07

### Breaking Changes

- `TypedArray::as_bytes`, `ArrayBuffer::as_bytes` and `ArrayBuffer::as_slice` are now `unsafe fn`, and the unsound safe `AsRef<[T]>` impls for `TypedArray` and `ArrayBuffer` were removed in favor of the new `unsafe fn TypedArray::as_slice` #[736](https://github.com/DelSkayn/rquickjs/issues/736)
- `TypedArray::as_raw` and `ArrayBuffer::as_raw` now return `NonNull<[u8]>` slice pointers, and the unnameable `RawArrayBuffer` type has been removed.

### Added

- Add pre-generated bindings for `riscv64gc-unknown-linux-gnu` and `riscv64a23-unknown-linux-gnu`
- `ArrayBuffer::new`-backed buffers now support `ArrayBuffer.prototype.transfer` to a different length instead of always failing

### Fixed

- Fixed deadlock when dropping a `Context` while the runtime lock is held under the `parallel` feature
- Updated `sys/quickjs` to `2c620e4` (quickjs-ng 0.16.2) and adapted `ArrayBuffer` callbacks to the new `JSReallocArrayBufferDataFunc` contract
- Removed stale `RefCountHeader` cast in `drop_context` (field moved to allocator header in quickjs-ng)

### Security

- Fixed `TypedArray`/`ArrayBuffer` byte-slice accessors returning a safe `&[u8]` that could be mutated, detached or reallocated by JS running while the slice was still borrowed #[736](https://github.com/DelSkayn/rquickjs/issues/736)

## [0.12.2] - 2026-07-27

### Added

- Added `TypedArray<U8Clamped>` support for `Uint8ClampedArray`, obtainable via `TypedArray::from_object` and matched by `is_typed_array` like other typed array classes

### Fixed

- Fixed `rquickjs-sys` host build failing during cross-compilation by reusing bundled bindings for the host target instead of running bindgen with a mismatched `libclang` #[709](https://github.com/DelSkayn/rquickjs/issues/709)

## [0.12.1] - 2026-06-29

### Changed

- Updated `phf`, `phf_shared` and `phf_generator` to 0.14

### Fixed

- Fixed `Constructor::new_prototype` leaking a reference cycle that aborted `JS_FreeRuntime` with a `gc_obj_list` assertion failure

## [0.12.0] - 2026-05-26

### Breaking Changes

- Added import attributes to the Loader trait #[601](https://github.com/DelSkayn/rquickjs/pull/601)
- Added half support (f16) for Float16Array # [662](https://github.com/DelSkayn/rquickjs/pull/662)
- `Value::new_big_int` now returns `Result<Value>` so QuickJS allocation failures are surfaced to the caller #[585](https://github.com/DelSkayn/rquickjs/issues/585)

### Added

- Added `#[qjs(prop)]` method attribute to declare a JS data property on a class prototype.
  Unlike `#[qjs(get)]`, which generates an accessor and requires a `FromJs<Self>` receiver,
  `prop` is evaluated at class registration time and emits a plain data descriptor (matching
  Web IDL semantics for `@@toStringTag`, `@@species`, etc.). Honours `rename`, `configurable`,
  `enumerable`, and a new `writable` flag (which also gates `writable` on `prop`).
- Added `FromJs` and `IntoJs` derive macros for plain-data structs
- Added `RQUICKJS_SYS_NO_WASI_SDK` env variable that skips downloading and setting up the WASI SDK when set to `1` #[648](https://github.com/DelSkayn/rquickjs/pull/648)
- Added `Object::new_proto` for creating objects with a custom or null prototype #[572](https://github.com/DelSkayn/rquickjs/issues/572)
- Added `Symbol::new`, `Symbol::with_description`, and `Symbol::new_global` for creating local and global symbols from Rust #[672](https://github.com/DelSkayn/rquickjs/pull/672)
- Added `ArrayBufferSource` trait and `ArrayBuffer::from_source` / `from_source_shared` / `from_source_immutable` safe constructors for wrapping external, caller-owned buffers, with built-in impls for `Vec<u8>`, `Box<[u8]>`, `Arc<[u8]>`, `Arc<Vec<u8>>`, and (behind the `bytes` feature) `bytes::Bytes`
- Add pre-generated bindings for `armv7-unknown-linux-gnueabihf`
- Add pre-generated bindings for `powerpc64-unknown-linux-gnu`

### Changed

- Bump MSRV to 1.87
- Updated `AsyncContext::async_with` to use async closure syntax #[602](https://github.com/DelSkayn/rquickjs/pull/602)

### Deprecated

- Deprecated `async_with!` macro #[602](https://github.com/DelSkayn/rquickjs/pull/602)

### Fixed

- Fixed `Property::configurable()` / `.writable()` / `.enumerable()` not emitting `JS_PROP_HAS_*` flags, so redefining an existing property silently kept its old attributes #[693](https://github.com/DelSkayn/rquickjs/issues/693)
- Fixed QuickJS rope strings (`JS_TAG_STRING_ROPE`) not being recognized as strings, causing `type_of()`, `is_string()`, and `as_string()` to fail on large concatenated strings
- Fixed interrupt handler causing GC assertion failure due to missing `JS_DupContext` on error context from `JS_ExecutePendingJob` #[664](https://github.com/DelSkayn/rquickjs/pull/664)
- Fixed cross-thread stack overflow false positives in parallel mode by updating stack baseline before QuickJS C entry points
- Fixed promise polling not returning Ready variant when exception occurs
- Fixed promise future aborting on stale pending exceptions by only bailing on uncatchable errors (e.g. interrupt handler)
- Fixed iterators to use correct IteratorPrototype chain
- Fixed a latent ABI layout vulnerability in `JS_NewPromiseCapability` FFI boundary by replacing tuple with strictly compatible array
- `#[rquickjs::class]` now rejects fields whose type implements `JsClass` with a clear compile error; such fields silently dropped nested mutations because the generated getter cloned the value. Wrap the field in `Class<'js, T>` instead #[532](https://github.com/DelSkayn/rquickjs/issues/532)
- Added missing `Trace` implementations for `Constructor`, `Promise`, `Proxy`, `ArrayBuffer`, and `TypedArray`; added `JsLifetime` for `Proxy`; re-exported `Constructor` at the crate root
- Fixed `#[qjs(static, rename = PredefinedAtom::...)]` methods failing when the target symbol exists as a non-writable property on `Function.prototype` (e.g. `Symbol.hasInstance`) by defining properties directly on the constructor instead of going through the prototype chain #[315](https://github.com/DelSkayn/rquickjs/issues/315)
- Fixed `#[qjs(static, get/set)]` accessors being placed on the class prototype instead of the constructor #[478](https://github.com/DelSkayn/rquickjs/issues/478)
- Fixed `Runtime::set_max_stack_size` crashing on large values (e.g. `usize::MAX`) by clamping to avoid a pointer underflow inside QuickJS's stack limit check #[437](https://github.com/DelSkayn/rquickjs/issues/437)

## [0.11.0] - 2025-12-16

### Added

- Added `Proxy` object #[#570](https://github.com/DelSkayn/rquickjs/pull/570)
- Allow setting filename as an eval option #[#536](https://github.com/DelSkayn/rquickjs/pull/536)
- Add Iterable allow JS to iterate over Rust iterator #[#564](https://github.com/DelSkayn/rquickjs/pull/564)
- Add JsIterator to iterate over Javascript Iterator #[#564](https://github.com/DelSkayn/rquickjs/pull/564)
- Add more trait implementations like AsRef for CString #[#558](https://github.com/DelSkayn/rquickjs/pull/558)

### Changed

- Bump MSRV to 1.85 #[#531](https://github.com/DelSkayn/rquickjs/pull/531)
- Update quickjs-ng to #[be664ca](https://github.com/quickjs-ng/quickjs/commit/be664ca1ccd86cbadf8df7348b8e15e77a32a1ba) #[#566](https://github.com/DelSkayn/rquickjs/pull/566)

### Fixed

- Fix wasm32 build #[548](https://github.com/DelSkayn/rquickjs/pull/548)

## [0.10.0] - 2025-10-24

### Added

- Allow `rquickjs-core` to build with `no_std` #[#455](https://github.com/DelSkayn/rquickjs/pull/455)
- Add `PromiseHook` bindings #[#453](https://github.com/DelSkayn/rquickjs/pull/453)
- Add `Module::write` options to enable features like `JS_WRITE_OBJ_STRIP_SOURCE` and `JS_WRITE_OBJ_STRIP_DEBUG` #[#443](https://github.com/DelSkayn/rquickjs/pull/443) and #[#518](https://github.com/DelSkayn/rquickjs/pull/518)
- Allow building with [sanitizer](https://doc.rust-lang.org/beta/unstable-book/compiler-flags/sanitizer.html) #[#425](https://github.com/DelSkayn/rquickjs/pull/425)
- Add `set_host_promise_rejection_tracker` to `AsyncRuntime` #[#452](https://github.com/DelSkayn/rquickjs/pull/452)
- Add linux arm64 support to `sys` crate #[#445](https://github.com/DelSkayn/rquickjs/pull/445)
- Implement `Trace` for `Atom` #[#517](https://github.com/DelSkayn/rquickjs/pull/517)
- Add `disable-assertions` feature to disable runtime assertions in quickjs #[#535](https://github.com/DelSkayn/rquickjs/pull/535)
- Add `JS_TAG_SHORT_BIG_INT` tag to support short BigInt #[#458](https://github.com/DelSkayn/rquickjs/pull/458) and #[#519](https://github.com/DelSkayn/rquickjs/pull/519)

### Changed

- Bump MSRV to 1.82 #[#531](https://github.com/DelSkayn/rquickjs/pull/531)
- Update to 2024 edition #[#473](https://github.com/DelSkayn/rquickjs/pull/473)
- Export `DriveFuture` #[#491](https://github.com/DelSkayn/rquickjs/pull/491)
- Switch to `dlopen2` for native module loading #[#513](https://github.com/DelSkayn/rquickjs/pull/513)

### Fixed

- Fix base objects intrinsic not being enabled in async context #[#442](https://github.com/DelSkayn/rquickjs/pull/442)
- Fix namespace resolution for JsLifetime in derive macro #[#429](https://github.com/DelSkayn/rquickjs/pull/429)
- Fix ownership of ctx pointers to be more ergonomic and fix cleanup bugs #[#433](https://github.com/DelSkayn/rquickjs/pull/433)
- Fix rename option in qjs attribute #[#428](https://github.com/DelSkayn/rquickjs/pull/428)
- Strip llvm suffix for `*-pc-windows-gnullvm` target #[#506](https://github.com/DelSkayn/rquickjs/pull/506)

## [0.9.0] - 2025-01-29

### Breaking Changes

- Switching to quickjs-ng from quickjs as base engine #[369](https://github.com/DelSkayn/rquickjs/pull/369)

This change should bring better performances and features, we are aware that it is bigger in intruction sizes.

We would eventually like to support the "original" quickjs engine again in the future, but we use new functions now that are only available in the `-ng` version.
